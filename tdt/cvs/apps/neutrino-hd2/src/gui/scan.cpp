/*
	Neutrino-GUI  -   DBoxII-Project

	Copyright (C) 2001 Steffen Hehn 'McClean'
	Homepage: http://dbox.cyberphoria.org/

	Kommentar:

	Diese GUI wurde von Grund auf neu programmiert und sollte nun vom
	Aufbau und auch den Ausbaumoeglichkeiten gut aussehen. Neutrino basiert
	auf der Client-Server Idee, diese GUI ist also von der direkten DBox-
	Steuerung getrennt. Diese wird dann von Daemons uebernommen.


	License: GPL

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gui/scale.h>
#include <gui/scan.h>

#include <driver/rcinput.h>
#include <driver/screen_max.h>

#include <gui/color.h>

#include <gui/widget/menue.h>
#include <gui/widget/messagebox.h>

#include <system/settings.h>

#include <global.h>
#include <neutrino.h>

#include <zapit/satconfig.h>
#include <zapit/frontend_c.h>

#include <gui/pictureviewer.h>

extern CPictureViewer * g_PicViewer;

#define NEUTRINO_SCAN_START_SCRIPT	CONFIGDIR "/scan.start"
#define NEUTRINO_SCAN_STOP_SCRIPT	CONFIGDIR "/scan.stop"
#define NEUTRINO_SCAN_SETTINGS_FILE	CONFIGDIR "/scan.conf"

TP_params TP;

#define RED_BAR 40
#define YELLOW_BAR 70
#define GREEN_BAR 100
#define BAR_BORDER 2
#define BAR_WIDTH 150
#define BAR_HEIGHT 16//(13 + BAR_BORDER*2)



CScanTs::CScanTs(int num)
{
	frameBuffer = CFrameBuffer::getInstance();
	radar = 0;
	total = done = 0;
	freqready = 0;

	sigscale = new CScale(BAR_WIDTH, BAR_HEIGHT, RED_BAR, GREEN_BAR, YELLOW_BAR);
	snrscale = new CScale(BAR_WIDTH, BAR_HEIGHT, RED_BAR, GREEN_BAR, YELLOW_BAR);
	
	//canceled = false;
	
	feindex = num;

}

extern int scan_pids;		// defined in zapit.cpp
#define get_set CNeutrinoApp::getInstance()->getScanSettings()

int CScanTs::exec(CMenuTarget* parent, const std::string & actionKey)
{
	diseqc_t            diseqcType = NO_DISEQC;
	neutrino_msg_t      msg;
	neutrino_msg_data_t data;
	//bool manual = (get_set.scan_mode == 2);
	int scan_mode = get_set.scan_mode;
	bool scan_all = actionKey == "all";
	bool test = actionKey == "test";
	bool manual = (actionKey == "manual") || test;
	
	sat_iterator_t sit;
	CZapitClient::ScanSatelliteList satList;
	CZapitClient::commandSetScanSatelliteList sat;
	
	int _scan_pids = scan_pids;

	hheight     = g_Font[SNeutrinoSettings::FONT_TYPE_MENU_TITLE]->getHeight();
	mheight     = g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->getHeight();
	//width       = w_max(550, 0);
	int fw = g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->getWidth();
	width       = w_max(fw * 42, 0);
	height      = h_max(hheight + (10 * mheight), 0); //9 lines
	x = frameBuffer->getScreenX() + (frameBuffer->getScreenWidth() - width) / 2;
	y = frameBuffer->getScreenY() + (frameBuffer->getScreenHeight() - height) / 2;
	xpos_radar = x + 470;
	ypos_radar = y + hheight + (mheight >> 1);
	xpos1 = x + BORDER_LEFT;

	if(scan_all)
		scan_mode |= 0xFF00;

	sigscale->reset();
	snrscale->reset();

	if (!frameBuffer->getActive())
		return menu_return::RETURN_EXIT_ALL;

        g_Zapit->stopPlayBack();

	bool usedBackground = frameBuffer->getuseBackground();
	if (usedBackground)
		frameBuffer->saveBackgroundImage();
	
	frameBuffer->loadBackgroundPic("scan.jpg");
#ifdef FB_BLIT	
	frameBuffer->blit();
#endif	

	g_Sectionsd->setPauseScanning(true);

	CVFD::getInstance()->setMode(CVFD::MODE_MENU_UTF8);

	// refill sat list and set feparams for manuel scan
	satList.clear();

	if(manual || !scan_all) 
	{
		for(sit = satellitePositions.begin(); sit != satellitePositions.end(); sit++) 
		{
			if(!strcmp(sit->second.name.c_str(), get_set.satNameNoDiseqc)) 
			{
				sat.position = sit->first;
				strncpy(sat.satName, get_set.satNameNoDiseqc, 50);
				// feindex
				sat.feindex = feindex;
			
				satList.push_back(sat);
				break;
			}
		}
		
		// scan pids
		scan_pids = true;
		
		// scan mode
		TP.scan_mode = get_set.scan_mode;
		
		// freq
		TP.feparams.frequency = atoi(get_set.TP_freq);
		
		if( CFrontend::getInstance(feindex)->getInfo()->type == FE_QPSK )
		{
			TP.feparams.u.qpsk.symbol_rate = atoi(get_set.TP_rate);
			TP.feparams.u.qpsk.fec_inner = (fe_code_rate_t) get_set.TP_fec;
			TP.polarization = get_set.TP_pol;
			//TP.feparams.u.qpsk.modulation	= (fe_modulation_t) get_set.TP_mod; //do we need this

			printf("CScanTs::exec: fe(%d) freq %d rate %d fec %d pol %d\n", feindex, TP.feparams.frequency, TP.feparams.u.qpsk.symbol_rate, TP.feparams.u.qpsk.fec_inner, TP.polarization/*, TP.feparams.u.qpsk.modulation*/ );
		} 
		else if( CFrontend::getInstance(feindex)->getInfo()->type == FE_QAM )
		{
			TP.feparams.u.qam.symbol_rate	= atoi(get_set.TP_rate);
			TP.feparams.u.qam.fec_inner	= (fe_code_rate_t)get_set.TP_fec;
			TP.feparams.u.qam.modulation	= (fe_modulation_t) get_set.TP_mod;

			printf("CScanTs::exec: fe(%d) freq %d rate %d fec %d mod %d\n", feindex, TP.feparams.frequency, TP.feparams.u.qam.symbol_rate, TP.feparams.u.qam.fec_inner, TP.feparams.u.qam.modulation);
		}
		else if( CFrontend::getInstance(feindex)->getInfo()->type == FE_OFDM )
		{
			TP.feparams.u.ofdm.bandwidth =  (fe_bandwidth_t)get_set.TP_band;
			TP.feparams.u.ofdm.code_rate_HP = (fe_code_rate_t)get_set.TP_HP; 
			TP.feparams.u.ofdm.code_rate_LP = (fe_code_rate_t)get_set.TP_LP; 
			TP.feparams.u.ofdm.constellation = (fe_modulation_t)get_set.TP_const; 
			TP.feparams.u.ofdm.transmission_mode = (fe_transmit_mode_t)get_set.TP_trans;
			TP.feparams.u.ofdm.guard_interval = (fe_guard_interval_t)get_set.TP_guard;
			//TP.feparams.u.ofdm.hierarchy_information = HIERARCHY_AUTO;
			TP.feparams.u.ofdm.hierarchy_information = (fe_hierarchy_t)get_set.TP_hierarchy;

			printf("CScanTs::exec: fe(%d) freq %d band %d HP %d LP %d const %d trans %d guard %d hierarchy %d\n", feindex, TP.feparams.frequency, TP.feparams.u.ofdm.bandwidth, TP.feparams.u.ofdm.code_rate_HP, TP.feparams.u.ofdm.code_rate_LP, TP.feparams.u.ofdm.constellation, TP.feparams.u.ofdm.transmission_mode, TP.feparams.u.ofdm.guard_interval, TP.feparams.u.ofdm.hierarchy_information);
		}
	} 
	else 
	{
		for(sit = satellitePositions.begin(); sit != satellitePositions.end(); sit++) 
		{
			if(sit->second.use_in_scan) 
			{
				// pos
				sat.position = sit->first;
				// name
				strncpy(sat.satName, sit->second.name.c_str(), 50);
				// feindex
				sat.feindex = feindex;
				
				satList.push_back(sat);
			}
		}
	}
	
	success = false;

	if(!manual) 
	{
                if (system(NEUTRINO_SCAN_START_SCRIPT) != 0)
                	perror(NEUTRINO_SCAN_START_SCRIPT " failed");
	}

	if( CFrontend::getInstance(feindex)->getInfo()->type == FE_QPSK )
	{
		// send diseqc type to zapit
		diseqcType = (diseqc_t) CNeutrinoApp::getInstance()->getScanSettings().diseqcMode;
		
		g_Zapit->setDiseqcType(diseqcType, feindex);
		printf("scan.cpp send to zapit diseqctype: %d\n", diseqcType);
			
		// send diseqc repeat to zapit
		g_Zapit->setDiseqcRepeat( CNeutrinoApp::getInstance()->getScanSettings().diseqcRepeat);
	}
	
	// send bouquets mode
	g_Zapit->setScanBouquetMode( (CZapitClient::bouquetMode)CNeutrinoApp::getInstance()->getScanSettings().bouquetMode);

	// send satellite list to zapit
	g_Zapit->setScanSatelliteList(satList);

        // send scantype to zapit
        g_Zapit->setScanType((CZapitClient::scanType) CNeutrinoApp::getInstance()->getScanSettings().scanType );
	
	paint(test);
	
#ifdef FB_BLIT
	frameBuffer->blit();
#endif	

	// go
	if(test) 
	{
		int w = x + width - xpos2;
		char buffer[128];
		char * f, *s, *m;

		if( CFrontend::getInstance(feindex)->getInfo()->type == FE_QPSK) 
		{
			CFrontend::getInstance(feindex)->getDelSys(get_set.TP_fec, dvbs_get_modulation((fe_code_rate_t)get_set.TP_fec), f, s, m);

			sprintf(buffer, "%u %c %d %s %s %s", atoi(get_set.TP_freq)/1000, get_set.TP_pol == 0 ? 'H' : 'V', atoi(get_set.TP_rate)/1000, f, s, m);
		} 
		else if( CFrontend::getInstance(feindex)->getInfo()->type == FE_QAM) 
		{
			CFrontend::getInstance(feindex)->getDelSys(get_set.TP_fec, get_set.TP_mod, f, s, m);

			sprintf(buffer, "%u %d %s %s %s", atoi(get_set.TP_freq), atoi(get_set.TP_rate)/1000, f, s, m);
		}
		else if( CFrontend::getInstance(feindex)->getInfo()->type == FE_OFDM)
		{
			CFrontend::getInstance(feindex)->getDelSys(get_set.TP_HP, get_set.TP_const, f, s, m);

			sprintf(buffer, "%u %s %s %s", atoi(get_set.TP_freq)/1000, f, s, m);
		}

		paintLine(xpos2, ypos_cur_satellite, w - 95, get_set.satNameNoDiseqc);
		paintLine(xpos2, ypos_frequency, w, buffer);

		success = g_Zapit->tune_TP(TP, feindex);
	} 
	else if(manual)
		success = g_Zapit->scan_TP(TP, feindex);
	else
		success = g_Zapit->startScan(scan_mode, feindex);

	// poll for messages
	istheend = !success;
	while (!istheend) 
	{
		paintRadar();

		unsigned long long timeoutEnd = CRCInput::calcTimeoutEnd_MS( 250 );

		do {
			g_RCInput->getMsgAbsoluteTimeout(&msg, &data, &timeoutEnd);

			if (test && (msg <= CRCInput::RC_MaxRC)) 
			{
				//TEST
				g_Zapit->Rezap();
				//
				istheend = true;
				msg = CRCInput::RC_timeout;
			}
			else if(msg == CRCInput::RC_home) 
			{
				// dont abort scan
				if(manual && get_set.scan_mode)
					continue;
				
				if (ShowLocalizedMessage(LOCALE_SCANTS_ABORT_HEADER, LOCALE_SCANTS_ABORT_BODY, CMessageBox::mbrNo, CMessageBox::mbYes | CMessageBox::mbNo) == CMessageBox::mbrYes) 
				{
					g_Zapit->stopScan();
					
					//canceled = true;
				}
			}
			else
				msg = handleMsg(msg, data);
		}
		while (!(msg == CRCInput::RC_timeout));
		
		showSNR(); // FIXME commented until scan slowdown will be solved
		
#ifdef FB_BLIT
		frameBuffer->blit();
#endif		
	}
	
	/* to join scan thread */
	g_Zapit->stopScan();

	if(!manual) 
	{
                if (system(NEUTRINO_SCAN_STOP_SCRIPT) != 0)
                	perror(NEUTRINO_SCAN_STOP_SCRIPT " failed");
				
	}

	if(!test) 
	{
		//ShowLocalizedMessage(LOCALE_MESSAGEBOX_INFO, success ? LOCALE_SCANTS_FINISHED : LOCALE_SCANTS_FAILED, CMessageBox::mbrBack, CMessageBox::mbBack, "info.raw");

		const char * text = g_Locale->getText(success ? LOCALE_SCANTS_FINISHED : LOCALE_SCANTS_FAILED);
		//paintLine(xpos2, ypos_frequency, xpos_frequency, text);
		
		// head
		frameBuffer->paintBoxRel(x, y, width, hheight, COL_MENUHEAD_PLUS_0, RADIUS_MID, CORNER_TOP);
		
		// exit icon
		int icon_hm_w, icon_hm_h;
		frameBuffer->getIconSize(NEUTRINO_ICON_BUTTON_HOME, &icon_hm_w, &icon_hm_h);
		frameBuffer->paintIcon(NEUTRINO_ICON_BUTTON_HOME, x + width - BORDER_RIGHT - icon_hm_w, y + 5);
		
		// setup icon
		int icon_s_w, icon_s_h;
		frameBuffer->getIconSize(NEUTRINO_ICON_SETTINGS, &icon_s_w, &icon_s_h);
		frameBuffer->paintIcon(NEUTRINO_ICON_SETTINGS,x + BORDER_LEFT, y + 8);
		
		// title
		g_Font[SNeutrinoSettings::FONT_TYPE_MENU_TITLE]->RenderString(xpos1 + 5 + icon_s_w, y + hheight, width - BORDER_RIGHT - BORDER_LEFT - icon_hm_w - icon_s_w, text, COL_MENUHEAD, 0, true); // UTF-8
		
		// exit icon
		//frameBuffer->paintIcon(NEUTRINO_ICON_BUTTON_HOME, x + width- 30, y + 5);
		
#ifdef FB_BLIT
		frameBuffer->blit();
#endif		
	
		unsigned long long timeoutEnd = CRCInput::calcTimeoutEnd(0xFFFF);

		do {
			g_RCInput->getMsgAbsoluteTimeout(&msg, &data, &timeoutEnd);
			if ( msg <= CRCInput::RC_MaxRC )
				msg = CRCInput::RC_timeout;
			else
				CNeutrinoApp::getInstance()->handleMsg( msg, data );
		} while (!(msg == CRCInput::RC_timeout));
	}

	hide();
	
	scan_pids = _scan_pids;
	
	// Restore previous background
	if (usedBackground)
		frameBuffer->restoreBackgroundImage();
	
	frameBuffer->useBackground(usedBackground);
	frameBuffer->paintBackground();
	
#ifdef FB_BLIT
	frameBuffer->blit();
#endif
	
	/* start sectionsd */
	g_Sectionsd->setPauseScanning(false);
	
	CVFD::getInstance()->setMode(CVFD::MODE_TVRADIO);

	return menu_return::RETURN_REPAINT;
}

int CScanTs::handleMsg(neutrino_msg_t msg, neutrino_msg_data_t data)
{
	int w = x + width - xpos2;
	
	//printf("CScanTs::handleMsg: x %d xpos2 %d width %d w %d\n", x, xpos2, width, w);
	
	char buffer[128];
	char str[256];

	switch (msg) 
	{
		case NeutrinoMessages::EVT_SCAN_SATELLITE:
			paintLine(xpos2, ypos_cur_satellite, w - 95, (char *)data);
			break;
			
		case NeutrinoMessages::EVT_SCAN_NUM_TRANSPONDERS:
			sprintf(buffer, "%d", data);
			paintLine(xpos2, ypos_transponder, w - 95, buffer);
			total = data;
			snprintf(str, 255, "scan: %d/%d", done, total);
#if 0
			CVFD::getInstance()->showMenuText(0, str, -1, true);
#endif			
			break;
			
		case NeutrinoMessages::EVT_SCAN_REPORT_NUM_SCANNED_TRANSPONDERS:
			if (total == 0) data = 0;
			done = data;
			sprintf(buffer, "%d/%d", done, total);
			paintLine(xpos2, ypos_transponder, w - 95, buffer);
			snprintf(str, 255, "scan %d/%d", done, total);
#if 0
			CVFD::getInstance()->showMenuText(0, str, -1, true);
#endif			
			break;

		case NeutrinoMessages::EVT_SCAN_REPORT_FREQUENCY:
			freqready = 1;
			sprintf(buffer, "%u", data);
			xpos_frequency = g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->getRenderWidth(buffer, true);
			paintLine(xpos2, ypos_frequency, xpos_frequency, buffer);
			break;
			
		case NeutrinoMessages::EVT_SCAN_REPORT_FREQUENCYP: 
			{
				int pol = data & 0xFF;
				int fec = (data >> 8) & 0xFF;
				int rate = data >> 16;
				char * f, *s, *m;
				
				CFrontend::getInstance(feindex)->getDelSys(fec, (fe_modulation_t)0, f, s, m); // FIXME
				
				sprintf(buffer, " %c %d %s %s %s", pol == 0 ? 'H' : 'V', rate, f, s, m);
				
				//(pol == 0) ? sprintf(buffer, "-H") : sprintf(buffer, "-V");
				paintLine(xpos2 + xpos_frequency, ypos_frequency, w - xpos_frequency - 80, buffer);
			}
			break;
			
		case NeutrinoMessages::EVT_SCAN_PROVIDER:
			paintLine(xpos2, ypos_provider, w, (char*)data); // UTF-8
			break;
			
		case NeutrinoMessages::EVT_SCAN_SERVICENAME:
			paintLine(xpos2, ypos_channel, w, (char *)data); // UTF-8
			break;
			
		case NeutrinoMessages::EVT_SCAN_NUM_CHANNELS:
			sprintf(buffer, " = %d", data);
			paintLine(xpos1 + 3 * 72, ypos_service_numbers + mheight, width - 3 * 72 - 10, buffer);
			break;
			
		case NeutrinoMessages::EVT_SCAN_FOUND_TV_CHAN:
			sprintf(buffer, "%d", data);
			paintLine(xpos1, ypos_service_numbers + mheight, 72, buffer);
			break;
			
		case NeutrinoMessages::EVT_SCAN_FOUND_RADIO_CHAN:
			sprintf(buffer, "%d", data);
			paintLine(xpos1 + 72, ypos_service_numbers + mheight, 72, buffer);
			break;
			
		case NeutrinoMessages::EVT_SCAN_FOUND_DATA_CHAN:
			sprintf(buffer, "%d", data);
			paintLine(xpos1 + 2 * 72, ypos_service_numbers + mheight, 72, buffer);
			break;
			
		case NeutrinoMessages::EVT_SCAN_COMPLETE:
		case NeutrinoMessages::EVT_SCAN_FAILED:
			success = (msg == NeutrinoMessages::EVT_SCAN_COMPLETE);
			istheend = true;
			msg = CRCInput::RC_timeout;
			break;
			
		case CRCInput::RC_plus:
		case CRCInput::RC_minus:
		case CRCInput::RC_left:
		case CRCInput::RC_right:
			CNeutrinoApp::getInstance()->setVolume(msg, true, true);
			break;
			
		default:
			if ((msg >= CRCInput::RC_WithData) && (msg < CRCInput::RC_WithData + 0x10000000)) 
				delete (unsigned char*) data;
			break;
	}
	
	return msg;
}

void CScanTs::paintRadar(void)
{
	char filename[30];
	
	sprintf(filename, "radar%d.raw", radar);
	radar = (radar + 1) % 10;
	frameBuffer->paintIcon8(filename, xpos_radar, ypos_radar, 17);
}

void CScanTs::hide()
{
	frameBuffer->paintBackground();
#ifdef FB_BLIT
	frameBuffer->blit();
#endif

	freqready = 0;
}

void CScanTs::paintLineLocale(int x, int * y, int width, const neutrino_locale_t l)
{
	frameBuffer->paintBoxRel(x, *y, width, mheight, COL_MENUCONTENT_PLUS_0);
	g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->RenderString(x, *y + mheight, width, g_Locale->getText(l), COL_MENUCONTENTINACTIVE, 0, true); // UTF-8
	*y += mheight;
}

void CScanTs::paintLine(int x, int y, int w, const char * const txt)
{
	//printf("CScanTs::paintLine x %d y %d w %d width %d xpos2 %d: %s\n", x, y, w, width, xpos2, txt);
	frameBuffer->paintBoxRel(x, y, w, mheight, COL_MENUCONTENT_PLUS_0);
	g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->RenderString(x, y + mheight, w, txt, COL_MENUCONTENT, 0, true); // UTF-8
}

void CScanTs::paint(bool fortest)
{
	int ypos;

	ypos = y;
	
	/* head */
	frameBuffer->paintBoxRel(x, ypos, width, hheight, COL_MENUHEAD_PLUS_0, RADIUS_MID, CORNER_TOP);
	
	/* icon */
	frameBuffer->paintIcon(NEUTRINO_ICON_SETTINGS,x + 8, ypos + 8);
	
	/* head title */
	g_Font[SNeutrinoSettings::FONT_TYPE_MENU_TITLE]->RenderString(xpos1 + 38, ypos + hheight, width, fortest ? g_Locale->getText(LOCALE_SCANTS_TEST) : g_Locale->getText(LOCALE_SCANTS_HEAD), COL_MENUHEAD, 0, true); // UTF-8
	
	/* exit icon */
	frameBuffer->paintIcon(NEUTRINO_ICON_BUTTON_HOME, x+ width- 30, ypos + 5);
	
	/* main box */
	frameBuffer->paintBoxRel(x, ypos + hheight, width, height - hheight, COL_MENUCONTENT_PLUS_0, RADIUS_MID, CORNER_BOTTOM);
	
	frameBuffer->loadPal("radar.pal", 17, 37);
	 
	ypos = y + hheight + (mheight >> 1);
	
	ypos_cur_satellite = ypos;
	

	if ( CFrontend::getInstance(feindex)->getInfo()->type == FE_QPSK)
	{	//sat
		paintLineLocale(xpos1, &ypos, width - xpos1, LOCALE_SCANTS_ACTSATELLITE);
		xpos2 = xpos1 + 10 + g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->getRenderWidth(g_Locale->getText(LOCALE_SCANTS_ACTSATELLITE), true); // UTF-8
	}

	//CABLE
	else if (CFrontend::getInstance(feindex)->getInfo()->type == FE_QAM)
	{	//cable
		paintLineLocale(xpos1, &ypos, width - xpos1, LOCALE_SCANTS_ACTCABLE);
		xpos2 = xpos1 + 10 + g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->getRenderWidth(g_Locale->getText(LOCALE_SCANTS_ACTCABLE), true); // UTF-8
	}

	//DVB-T
	else if (CFrontend::getInstance(feindex)->getInfo()->type == FE_OFDM)
	{	//terrestrial
		paintLineLocale(xpos1, &ypos, width - xpos1, LOCALE_SCANTS_ACTTERRESTRIAL);
		xpos2 = xpos1 + 10 + g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->getRenderWidth(g_Locale->getText(LOCALE_SCANTS_ACTTERRESTRIAL), true); // UTF-8
	}

	ypos_transponder = ypos;
	paintLineLocale(xpos1, &ypos, width - xpos1, LOCALE_SCANTS_TRANSPONDERS);
	xpos2 = greater_xpos(xpos2, LOCALE_SCANTS_TRANSPONDERS);

	ypos_frequency = ypos;
	paintLineLocale(xpos1, &ypos, width - xpos1, LOCALE_SCANTS_FREQDATA);
	xpos2 = greater_xpos(xpos2, LOCALE_SCANTS_FREQDATA);

	ypos += mheight >> 1; // 1/2 blank line
	
	ypos_provider = ypos;
	paintLineLocale(xpos1, &ypos, width - xpos1, LOCALE_SCANTS_PROVIDER);
	xpos2 = greater_xpos(xpos2, LOCALE_SCANTS_PROVIDER);
	
	ypos_channel = ypos;
	paintLineLocale(xpos1, &ypos, width - xpos1, LOCALE_SCANTS_CHANNEL);
	xpos2 = greater_xpos(xpos2, LOCALE_SCANTS_CHANNEL);

	ypos += mheight >> 1; // 1/2 blank line

	ypos_service_numbers = ypos; paintLineLocale(xpos1         , &ypos, 72                 , LOCALE_SCANTS_NUMBEROFTVSERVICES   );
	ypos = ypos_service_numbers; paintLineLocale(xpos1 +     72, &ypos, 72                 , LOCALE_SCANTS_NUMBEROFRADIOSERVICES);
	ypos = ypos_service_numbers; paintLineLocale(xpos1 + 2 * 72, &ypos, 72                 , LOCALE_SCANTS_NUMBEROFDATASERVICES );
	ypos = ypos_service_numbers; paintLineLocale(xpos1 + 3 * 72, &ypos, width - 3 * 72 - 10, LOCALE_SCANTS_NUMBEROFTOTALSERVICES);
}

int CScanTs::greater_xpos(int xpos, const neutrino_locale_t txt)
{
	int txt_xpos = xpos1 + 10 + g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->getRenderWidth(g_Locale->getText(txt), true); // UTF-8
	if (txt_xpos > xpos)
		return txt_xpos;
	else 
		return xpos;
}

void CScanTs::showSNR()
{
	char percent[10];
	int barwidth = 150;
	uint16_t ssig, ssnr;
	int sig, snr;
	int posx, posy;
	int sw;

	ssig = CFrontend::getInstance(feindex)->getSignalStrength();
	ssnr = CFrontend::getInstance(feindex)->getSignalNoiseRatio();

	snr = (ssnr & 0xFFFF) * 100 / 65535;
	sig = (ssig & 0xFFFF) * 100 / 65535;

	posy = y + height - mheight - 5;

	if(sigscale->getPercent() != sig) 
	{
		posx = x + 20;
		sprintf(percent, "%d%% SIG", sig);
		sw = g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->getRenderWidth ("100% SIG");

		sigscale->paint(posx - 1, posy+2, sig);

		posx = posx + barwidth + 3;
		sw = x + 247 - posx;
		frameBuffer->paintBoxRel(posx, posy - 2, sw, mheight, COL_MENUCONTENT_PLUS_0);
		g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->RenderString (posx+2, posy + mheight, sw, percent, COL_MENUCONTENT);
	}

	if(snrscale->getPercent() != snr) 
	{
		posx = x + 20 + 260;
		sprintf(percent, "%d%% SNR", snr);
		sw = g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->getRenderWidth ("100% SNR");
		snrscale->paint(posx - 1, posy+2, snr);

		posx = posx + barwidth + 3;
		sw = x + width - posx;
		frameBuffer->paintBoxRel(posx, posy - 2, sw, mheight, COL_MENUCONTENT_PLUS_0);
		g_Font[SNeutrinoSettings::FONT_TYPE_MENU]->RenderString (posx+2, posy + mheight, sw, percent, COL_MENUCONTENT);
	}
}
