/*
 * $Id: zapit.cpp,v 1.290.2.50 2003/06/13 10:53:15 digi_casi Exp $
 *
 * zapit - d-box2 linux project
 *
 * (C) 2001, 2002 by Philipp Leusmann <faralla@berlios.de>
 * (C) 2007-2012 Stefan Seyfried
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/* system headers */
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <syscall.h>

#include <pthread.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* zapit headers */
#include <zapit/cam.h>
#include <zapit/client/msgtypes.h>
#include <zapit/debug.h>
#include <zapit/getservices.h>
#include <zapit/pat.h>
#include <zapit/pmt.h>
#include <zapit/scan.h>
//#include <zapit/fastscan.h>
#include <zapit/settings.h>
#include <zapit/zapit.h>
#include <xmlinterface.h>

#include <ca_cs.h>

#include <zapit/satconfig.h>
#include <zapit/frontend_c.h>
#include <dmx.h>
#if HAVE_COOL_HARDWARE
#include <record_cs.h>
#include <playback_cs.h>
#include <pwrmngr.h>
#include <audio_cs.h>
#include <video_cs.h>
#endif
#if USE_STB_HAL || HAVE_TRIPLEDRAGON
#include <video_td.h>
#include <audio_td.h>
#endif

#include <driver/abstime.h>
#ifdef EVOLUX
#include <driver/rcinput.h>
#include <driver/volume.h>
#endif
#include "libdvbsub/dvbsub.h"
#include "libtuxtxt/teletext.h"

#ifdef EVOLUX
#define VOLUME_DEFAULT_PCM 75
#define VOLUME_DEFAULT_AC3 100
#endif

/* globals */

int sig_delay = 2; // seconds between signal check

/* the bouquet manager */
CBouquetManager *g_bouquetManager = NULL;

int cam_ci = 2; //  CA_INIT_SC 0 or CA_INIT_CI 1 or CA_INIT_BOTH 2
cCA *ca = NULL;
extern cDemux * pmtDemux;
extern cVideo *videoDecoder;
extern cAudio *audioDecoder;
extern cDemux *audioDemux;
extern cDemux *videoDemux;

cDemux *pcrDemux = NULL;

/* the map which stores the wanted cable/satellites */
scan_list_t scanProviders;

int zapit_debug = 0;

/* transponder scan */
transponder_list_t transponders;

CZapitClient::bouquetMode bouquetMode = CZapitClient::BM_UPDATEBOUQUETS;
CZapitClient::scanType scanType = CZapitClient::ST_TVRADIO;

//static TP_params TP;

static bool update_pmt = true;
/******************************************************************************/
CZapit * CZapit::zapit = NULL;
CZapit::CZapit() 
	: configfile(',', false)
{
	started = false;
	pmt_update_fd = -1;
	volume_left = 0, volume_right = 0;
	def_volume_left = 0, def_volume_right = 0;
	audio_mode = 0;
	aspectratio=0;
	mode43=0;
	def_audio_mode = 0;
	playbackStopForced = false;
	standby = true;
	event_mode = true;
	firstzap = true;
	playing = false;
	list_changed = false; // flag to indicate, allchans was changed
	currentMode = 0;
}

CZapit::~CZapit()
{
	Stop();
}

CZapit * CZapit::getInstance()
{
	if(zapit == NULL)
		zapit = new CZapit();
	return zapit;
}

void CZapit::SendEvent(const unsigned int eventID, const void* eventbody, const unsigned int eventbodysize)
{
	if(event_mode)
		eventServer->sendEvent(eventID, CEventServer::INITID_ZAPIT, eventbody, eventbodysize);
}

void CZapit::SaveSettings(bool write, bool write_a)
{
	if (current_channel) {
		// now save the lowest channel number with the current channel_id
		int c = ((currentMode & RADIO_MODE) ? g_bouquetManager->radioChannelsBegin() : g_bouquetManager->tvChannelsBegin()).getLowestChannelNumberWithChannelID(current_channel->getChannelID());
//printf("LAST CHAN %d !!!!!!!!!!!\n\n\n", c);
		if (c >= 0) {
			if ((currentMode & RADIO_MODE))
				lastChannelRadio = c;
			else
				lastChannelTV = c;
		}
	}

	if (write) {
		configfile.setBool("saveLastChannel", config.saveLastChannel);
		if (config.saveLastChannel) {
			configfile.setInt32("lastChannelMode", (currentMode & RADIO_MODE) ? 1 : 0);
			configfile.setInt32("lastChannelRadio", lastChannelRadio);
			configfile.setInt32("lastChannelTV", lastChannelTV);
			configfile.setInt64("lastChannel", live_channel_id);
#ifdef EVOLUX
			if (!(currentMode & RADIO_MODE) && (lastChannelTV >= 0))
				configfile.setBool("lastChannelTVScrambled", !current_channel || current_channel->scrambled);
#endif
		}
		configfile.setInt32("lastSatellitePosition", CFrontend::getInstance()->getCurrentSatellitePosition());
		configfile.setInt32("diseqcRepeats", CFrontend::getInstance()->getDiseqcRepeats());
		configfile.setInt32("diseqcType", CFrontend::getInstance()->getDiseqcType());

		configfile.setInt32("motorRotationSpeed", config.motorRotationSpeed);
		configfile.setBool("writeChannelsNames", config.writeChannelsNames);
		configfile.setBool("makeRemainingChannelsBouquet", config.makeRemainingChannelsBouquet);
#ifdef EVOLUX
		configfile.setBool("makeNewChannelsBouquet", config.makeNewChannelsBouquet);
#endif
		configfile.setInt32("feTimeout", config.feTimeout);

		configfile.setInt32("rezapTimeout", config.rezapTimeout);
		configfile.setBool("scanPids", config.scanPids);
		configfile.setBool("highVoltage", config.highVoltage);

		char tempd[12];
		sprintf(tempd, "%3.6f", config.gotoXXLatitude);
		configfile.setString("gotoXXLatitude", tempd);
		sprintf(tempd, "%3.6f", config.gotoXXLongitude);
		configfile.setString("gotoXXLongitude", tempd);
		configfile.setInt32("gotoXXLaDirection", config.gotoXXLaDirection);
		configfile.setInt32("gotoXXLoDirection", config.gotoXXLoDirection);
		configfile.setInt32("repeatUsals", config.repeatUsals);
		configfile.setInt32("scanSDT", config.scanSDT);

		configfile.setInt32("uni_scr", config.uni_scr);
		configfile.setInt32("uni_qrg", config.uni_qrg);

		configfile.setInt32("cam_ci", cam_ci);

#if 0 // unused
		configfile.setBool("fastZap", config.fastZap);
		configfile.setBool("sortNames", config.sortNames);
#endif
		if (configfile.getModifiedFlag())
			configfile.saveConfig(CONFIGFILE);

	}
        if (write_a) {
                FILE *audio_config_file = fopen(AUDIO_CONFIG_FILE, "w");
                if (audio_config_file) {
                  for (audio_map_iterator_t audio_map_it = audio_map.begin(); audio_map_it != audio_map.end(); audio_map_it++) {
                        fprintf(audio_config_file, "%llx %d %d %d %d %d %d\n", (uint64_t) audio_map_it->first,
                                (int) audio_map_it->second.apid, (int) audio_map_it->second.mode, (int) audio_map_it->second.volume, 
				(int) audio_map_it->second.subpid, (int) audio_map_it->second.ttxpid, (int) audio_map_it->second.ttxpage);
                  }
		  fflush(audio_config_file);
		  fdatasync(fileno(audio_config_file));
                  fclose(audio_config_file);
#ifdef EVOLUX
		  FILE *volume_config_file = fopen(VOLUME_CONFIG_FILE, "w");
		  if (volume_config_file) {
			for (volume_map_it = volume_map.begin(); volume_map_it != volume_map.end(); volume_map_it++) {
				fprintf(volume_config_file, "%llx %d %d\n", (uint64_t) volume_map_it->first.first,
					(int) volume_map_it->first.second, (int) volume_map_it->second);
			}
			fflush(volume_config_file);
			fdatasync(fileno(volume_config_file));
			fclose(volume_config_file);
		  }
#endif
                } else
			perror(AUDIO_CONFIG_FILE);
        }
}

void CZapit::LoadAudioMap()
{
	FILE *audio_config_file = fopen(AUDIO_CONFIG_FILE, "r");
	audio_map.clear();
	if (audio_config_file) {
		t_channel_id chan;
		int apid = 0, subpid = 0, ttxpid = 0, ttxpage = 0;
		int mode = 0;
		int volume = 0;
		char s[1000];
#ifdef EVOLUX
		while (fgets(s, sizeof(s), audio_config_file)) {
#else
		while (fgets(s, 1000, audio_config_file)) {
#endif
			sscanf(s, "%llx %d %d %d %d %d %d", &chan, &apid, &mode, &volume, &subpid, &ttxpid, &ttxpage);
			audio_map[chan].apid = apid;
			audio_map[chan].subpid = subpid;
			audio_map[chan].mode = mode;
			audio_map[chan].volume = volume;
			audio_map[chan].ttxpid = ttxpid;
			audio_map[chan].ttxpage = ttxpage;
		}
		fclose(audio_config_file);
	} else
		perror(AUDIO_CONFIG_FILE);

#ifdef EVOLUX
	FILE *volume_config_file = fopen(VOLUME_CONFIG_FILE, "r");
	volume_map.clear();
	if (volume_config_file) {
		t_channel_id chan;
		char s[1000];
		while (fgets(s, sizeof(s), volume_config_file)) {
			t_channel_id chan;
			int apid, percent;
			if (3 == sscanf(s, "%llx %d %d", &chan, &apid, &percent)) {
				volume_map[make_pair(chan, apid)] = percent;
			} 
		}
		fclose(volume_config_file);
	}
#endif
}

void CZapit::LoadSettings()
{
	if (!configfile.loadConfig(CONFIGFILE))
		WARN("%s not found", CONFIGFILE);

	live_channel_id				= configfile.getInt64("lastChannel", 0);
	lastChannelRadio			= configfile.getInt32("lastChannelRadio", 0);
	lastChannelTV				= configfile.getInt32("lastChannelTV", 0);

#if 0 //unused
	config.fastZap				= configfile.getBool("fastZap", 1);
	config.sortNames			= configfile.getBool("sortNames", 0);
	voltageOff				= configfile.getBool("voltageOff", 0);
#endif
	config.saveLastChannel			= configfile.getBool("saveLastChannel", true);
	config.rezapTimeout			= configfile.getInt32("rezapTimeout", 1);
	config.feTimeout			= configfile.getInt32("feTimeout", 40);
	config.scanPids				= configfile.getBool("scanPids", 0);
	config.highVoltage			= configfile.getBool("highVoltage", 0);
	config.makeRemainingChannelsBouquet	= configfile.getBool("makeRemainingChannelsBouquet", 1);
#ifdef EVOLUX
	config.makeNewChannelsBouquet		= configfile.getBool("makeNewChannelsBouquet", true);
#endif

	config.gotoXXLatitude 			= strtod(configfile.getString("gotoXXLatitude", "0.0").c_str(), NULL);
	config.gotoXXLongitude			= strtod(configfile.getString("gotoXXLongitude", "0.0").c_str(), NULL);
	config.gotoXXLaDirection		= configfile.getInt32("gotoXXLaDirection", 1);
	config.gotoXXLoDirection		= configfile.getInt32("gotoXXLoDirection", 0);
	config.repeatUsals			= configfile.getInt32("repeatUsals", 0);

	config.scanSDT				= configfile.getInt32("scanSDT", 0);

	config.uni_scr				= configfile.getInt32("uni_scr", -1);
	config.uni_qrg				= configfile.getInt32("uni_qrg", 0);

	cam_ci					= configfile.getInt32("cam_ci", 2);

	diseqcType				= (diseqc_t)configfile.getInt32("diseqcType", NO_DISEQC);
	config.motorRotationSpeed		= configfile.getInt32("motorRotationSpeed", 18); // default: 1.8 degrees per second

	CFrontend::getInstance()->setDiseqcRepeats(configfile.getInt32("diseqcRepeats", 0));
	CFrontend::getInstance()->setCurrentSatellitePosition(configfile.getInt32("lastSatellitePosition", 0));
	CFrontend::getInstance()->setDiseqcType(diseqcType);

	//FIXME globals !
#if 0
	motorRotationSpeed = config.motorRotationSpeed;
	highVoltage = config.highVoltage;
	feTimeout = config.feTimeout;
	gotoXXLaDirection = config.gotoXXLaDirection;
	gotoXXLoDirection = config.gotoXXLoDirection;
	gotoXXLatitude = config.gotoXXLatitude;
	gotoXXLongitude = config.gotoXXLongitude;
	repeatUsals = config.repeatUsals;
#endif
	printf("[zapit.cpp] diseqc type = %d\n", diseqcType);
	LoadAudioMap();
}

void CZapit::ConfigFrontend()
{
	CFrontend::getInstance()->setTimeout(config.feTimeout);
	CFrontend::getInstance()->configUsals(config.gotoXXLatitude, config.gotoXXLongitude,
		config.gotoXXLaDirection, config.gotoXXLoDirection, config.repeatUsals);
	CFrontend::getInstance()->configRotor(config.motorRotationSpeed, config.highVoltage);
	CFrontend::getInstance()->configUnicable(config.uni_scr, config.uni_qrg);
}

void CZapit::SendPMT(bool forupdate)
{
	if(!current_channel)
		return;

	CCamManager::getInstance()->Start(current_channel->getChannelID(), CCamManager::PLAY, forupdate);
	int len;
	unsigned char * pmt = current_channel->getRawPmt(len);
	ca->SendPMT(DEMUX_SOURCE_0, pmt, len);
}

void CZapit::SaveChannelPids(CZapitChannel* channel)
{
	if(channel == NULL)
		return;

	printf("[zapit] saving channel, apid %x sub pid %x mode %d volume %d\n", channel->getAudioPid(), dvbsub_getpid(), audio_mode, volume_right);
	audio_map[channel->getChannelID()].apid = channel->getAudioPid();
	audio_map[channel->getChannelID()].mode = audio_mode;
	audio_map[channel->getChannelID()].volume = audioDecoder->getVolume();
	audio_map[channel->getChannelID()].subpid = dvbsub_getpid();
	tuxtx_subtitle_running(&audio_map[channel->getChannelID()].ttxpid, &audio_map[channel->getChannelID()].ttxpage, NULL);
}

void CZapit::RestoreChannelPids(CZapitChannel * channel)
{
	audio_map_set_t * pidmap = GetSavedPids(channel->getChannelID());
	if(pidmap) {
		printf("[zapit] channel found, audio pid %x, subtitle pid %x mode %d volume %d\n",
				pidmap->apid, pidmap->subpid, pidmap->mode, pidmap->volume);

		if(channel->getAudioChannelCount() > 1) {
			for (int i = 0; i < channel->getAudioChannelCount(); i++) {
				if (channel->getAudioChannel(i)->pid == pidmap->apid ) {
					DBG("***** Setting audio!\n");
					channel->setAudioChannel(i);
#if 0
					if(we_playing && (channel->getAudioChannel(i)->audioChannelType != CZapitAudioChannel::MPEG))
						ChangeAudioPid(i);
#endif
				}
			}
		}
#if 0 // to restore saved volume per channel if needed. after first zap its done by neutrino
		if(firstzap) {
			audioDecoder->setVolume(audio_map_it->second.volume, audio_map_it->second.volume);
		}
#endif
		volume_left = volume_right = pidmap->volume;
		audio_mode = pidmap->mode;

		dvbsub_setpid(pidmap->subpid);

		std::string tmplang;
		for (int i = 0 ; i < (int)channel->getSubtitleCount() ; ++i) {
			CZapitAbsSub* s = channel->getChannelSub(i);
			if(s->pId == pidmap->ttxpid) {
				tmplang = s->ISO639_language_code;
				break;
			}
		}
		if(tmplang.empty())
			tuxtx_set_pid(pidmap->ttxpid, pidmap->ttxpage, (char *) channel->getTeletextLang());
		else
			tuxtx_set_pid(pidmap->ttxpid, pidmap->ttxpage, (char *) tmplang.c_str());
	} else {
		volume_left = volume_right = def_volume_left;
		audio_mode = def_audio_mode;
		tuxtx_set_pid(0, 0, (char *) channel->getTeletextLang());
	}
	/* restore saved stereo / left / right channel mode */
	//audioDecoder->setVolume(volume_left, volume_right);
#ifdef EVOLUX
	t_chan_apid chan_apid = make_pair(live_channel_id, channel->getAudioPid());
		volume_map_it = volume_map.find(chan_apid);
	if((volume_map_it != volume_map.end()) )
		audioDecoder->setPercent(volume_map_it->second);
	else if (channel->getAudioChannel()) {
		audioDecoder->setPercent(
			(channel->getAudioChannel()->audioChannelType == CZapitAudioChannel::AC3)
			? VOLUME_DEFAULT_AC3 : VOLUME_DEFAULT_PCM);
	}
#endif
	audioDecoder->setChannel(audio_mode);
}

audio_map_set_t * CZapit::GetSavedPids(const t_channel_id channel_id)
{
	audio_map_iterator_t audio_map_it = audio_map.find(channel_id);
	if(audio_map_it != audio_map.end())
		return &audio_map_it->second;

	return NULL;
}

bool CZapit::TuneChannel(CZapitChannel * channel, bool &transponder_change)
{
	transponder_change = false;
	if (!(currentMode & RECORD_MODE)) {
		transponder_change = CFrontend::getInstance()->setInput(channel, current_is_nvod);
		if(transponder_change && !current_is_nvod) {
			int waitForMotor = CFrontend::getInstance()->driveToSatellitePosition(channel->getSatellitePosition());
			if(waitForMotor > 0) {
				printf("[zapit] waiting %d seconds for motor to turn satellite dish.\n", waitForMotor);
				SendEvent(CZapitClient::EVT_ZAP_MOTOR, &waitForMotor, sizeof(waitForMotor));
				for(int i = 0; i < waitForMotor; i++) {
					sleep(1);
					if(abort_zapit) {
						abort_zapit = 0;
						return false;
					}
				}
			}
		}

		/* if channel's transponder does not match frontend's tuned transponder ... */
		if (transponder_change || current_is_nvod) {
			if (CFrontend::getInstance()->tuneChannel(channel, current_is_nvod) == false) {
				return false;
			}
		}
	} else if(!SAME_TRANSPONDER(channel->getChannelID(), live_channel_id))
		return false;

	return true;
}

bool CZapit::ParsePatPmt(CZapitChannel * channel)
{
	DBG("looking up pids for channel_id " PRINTF_CHANNEL_ID_TYPE "\n", channel->getChannelID());
	/* get program map table pid from program association table */
	if (channel->getPmtPid() == 0) {
		printf("[zapit] no pmt pid, going to parse pat\n");
		if (parse_pat(channel) < 0) {
			printf("[zapit] pat parsing failed\n");
			return false;
		}
	}

	/* parse program map table and store pids */
	if (parse_pmt(channel) < 0) {
		printf("[zapit] pmt parsing failed\n");
		if (parse_pat(channel) < 0) {
			printf("pat parsing failed\n");
			return false;
		}
		else if (parse_pmt(channel) < 0) {
			printf("[zapit] pmt parsing failed\n");
			return false;
		}
	}
	return true;
}

bool CZapit::ZapIt(const t_channel_id channel_id, bool forupdate, bool startplayback)
{
	bool transponder_change = false;
	bool failed = false;
	CZapitChannel* newchannel;

	DBG("[zapit] zapto channel id %llx diseqcType %d\n", channel_id, diseqcType);

	abort_zapit = 0;
	if((newchannel = CServiceManager::getInstance()->FindChannel(channel_id, &current_is_nvod)) == NULL) {
		DBG("channel_id " PRINTF_CHANNEL_ID_TYPE " not found", channel_id);
		return false;
	}

	sig_delay = 2;
	if (!firstzap && current_channel)
		SaveChannelPids(current_channel);

	/* firstzap right now does nothing but control saving the audio channel */
	firstzap = false;

	pmt_stop_update_filter(&pmt_update_fd);

	ca->SendPMT(0, (unsigned char*) "", 0, CA_SLOT_TYPE_CI);
	StopPlayBack(!forupdate);

	if(!forupdate && current_channel)
		current_channel->resetPids();

	current_channel = newchannel;

	live_channel_id = current_channel->getChannelID();
	SaveSettings(false, false);

	printf("[zapit] zap to %s (%llx)\n", current_channel->getName().c_str(), live_channel_id);

	if(!TuneChannel(newchannel, transponder_change))
		return false;

	if (current_channel->getServiceType() == ST_NVOD_REFERENCE_SERVICE) {
		current_is_nvod = true;
		return true;
	}
#if 0 //FIXME improve or remove ? this is to start playback before pmt
	//bool we_playing = channel->getPidsFlag();//FIXME: this starts playback before pmt
	bool we_playing = 0;

	if (we_playing) {
		printf("[zapit] channel have pids: vpid %X apid %X pcr %X\n", current_channel->getVideoPid(), current_channel->getPreAudioPid(), current_channel->getPcrPid());fflush(stdout);
		if((current_channel->getAudioChannelCount() <= 0) && current_channel->getPreAudioPid() > 0) {
			std::string desc = "Preset";
			current_channel->addAudioChannel(current_channel->getPreAudioPid(), CZapitAudioChannel::MPEG, desc, 0xFF);
		}
		StartPlayBack(current_channel);
	}
#endif

	failed = !ParsePatPmt(current_channel);

	if ((!failed) && (current_channel->getAudioPid() == 0) && (current_channel->getVideoPid() == 0)) {
		printf("[zapit] neither audio nor video pid found\n");
		failed = true;
	}

	/* start sdt scan even if the service was not found in pat or pmt
	 * if the frontend did not tune, we don't get here, so this is fine */
	if (transponder_change)
		SdtMonitor.Wakeup();

	if (failed)
		return false;

	current_channel->getCaPmt()->ca_pmt_list_management = transponder_change ? 0x03 : 0x04;

	RestoreChannelPids(current_channel);

	if (startplayback /* && !we_playing*/)
		StartPlayBack(current_channel);

	printf("[zapit] sending capmt....\n");

	SendPMT(forupdate);
	//play:
	int caid = 1;
	SendEvent(CZapitClient::EVT_ZAP_CA_ID, &caid, sizeof(int));

	if (update_pmt)
		pmt_set_update_filter(current_channel, &pmt_update_fd);

	return true;
}

bool CZapit::ZapForRecord(const t_channel_id channel_id)
{
	CZapitChannel* newchannel;
	bool transponder_change;

	if((newchannel = CServiceManager::getInstance()->FindChannel(channel_id)) == NULL) {
		printf("zapit_to_record: channel_id " PRINTF_CHANNEL_ID_TYPE " not found", channel_id);
		return false;
	}
	printf("%s: %s (%llx)\n", __FUNCTION__, newchannel->getName().c_str(), channel_id);
	if(!TuneChannel(newchannel, transponder_change))
		return false;

	if(!ParsePatPmt(newchannel))
		return false;

	return true;
}

void CZapit::SetAudioStreamType(CZapitAudioChannel::ZapitAudioChannelType audioChannelType)
{
	const char *audioStr = "UNKNOWN";
	switch (audioChannelType) {
		case CZapitAudioChannel::AC3:
			audioStr = "AC3";
			audioDecoder->SetStreamType(AUDIO_FMT_DOLBY_DIGITAL);
			break;
		case CZapitAudioChannel::MPEG:
			audioStr = "MPEG2";
			audioDecoder->SetStreamType(AUDIO_FMT_MPEG);
			break;
		case CZapitAudioChannel::AAC:
			audioStr = "AAC";
			audioDecoder->SetStreamType(AUDIO_FMT_AAC);
			break;
		case CZapitAudioChannel::AACPLUS:
			audioStr = "AAC-PLUS";
			audioDecoder->SetStreamType(AUDIO_FMT_AAC_PLUS);
			break;
		case CZapitAudioChannel::DTS:
			audioStr = "DTS";
			audioDecoder->SetStreamType(AUDIO_FMT_DTS);
			break;
		default:
			printf("[zapit] unknown audio channel type 0x%x\n", audioChannelType);
			break;
	}

	printf("[zapit] starting %s audio\n", audioStr);

#ifdef EVOLUX
	t_chan_apid chan_apid = make_pair(live_channel_id, current_channel->getAudioPid());
	volume_map_it = volume_map.find(chan_apid);
	if((volume_map_it != volume_map.end()) )
		audioDecoder->setPercent(volume_map_it->second);
	else
		audioDecoder->setPercent(
			(audioChannelType == CZapitAudioChannel::AC3)
			? VOLUME_DEFAULT_AC3 : VOLUME_DEFAULT_PCM);
#endif
}

bool CZapit::ChangeAudioPid(uint8_t index)
{
	if (!current_channel)
		return false;

	/* stop demux filter */
	if (audioDemux->Stop() < 0)
		return false;

	/* stop audio playback */
	if (audioDecoder->Stop() < 0)
		return false;

	/* update current channel */
	current_channel->setAudioChannel(index);

	/* set bypass mode */
	CZapitAudioChannel *currentAudioChannel = current_channel->getAudioChannel();

	if (!currentAudioChannel) {
		WARN("No current audio channel");
		return false;
	}

	printf("[zapit] change apid to 0x%x\n", current_channel->getAudioPid());
	SetAudioStreamType(currentAudioChannel->audioChannelType);

	/* set demux filter */
	if (audioDemux->pesFilter(current_channel->getAudioPid()) < 0)
		return false;

	/* start demux filter */
	if (audioDemux->Start() < 0)
		return false;

#ifdef EVOLUX
	t_chan_apid chan_apid = make_pair(live_channel_id, current_channel->getAudioPid());
	volume_map_it = volume_map.find(chan_apid);
	if (volume_map_it != volume_map.end())
		audioDecoder->setPercent(volume_map[chan_apid]);
	else
		audioDecoder->setPercent(
			(currentAudioChannel->audioChannelType == CZapitAudioChannel::AC3)
			? VOLUME_DEFAULT_AC3 : VOLUME_DEFAULT_PCM);
#endif

	/* start audio playback */
	if (audioDecoder->Start() < 0)
		return false;

	return true;
}

void CZapit::SetRadioMode(void)
{
	currentMode |= RADIO_MODE;
	currentMode &= ~TV_MODE;
}

void CZapit::SetTVMode(void)
{
	currentMode |= TV_MODE;
	currentMode &= ~RADIO_MODE;
}

int CZapit::getMode(void)
{
	int mode = currentMode & (~RECORD_MODE);
	return mode;

	if (currentMode & TV_MODE)
		return CZapitClient::MODE_TV;
	if (currentMode & RADIO_MODE)
		return CZapitClient::MODE_RADIO;
	return 0;
}

void CZapit::SetRecordMode(bool enable)
{
	unsigned int event;
	bool mode = currentMode & RECORD_MODE;

	if(mode == enable)
		return;

	if(enable) {
		currentMode |= RECORD_MODE;
		event = CZapitClient::EVT_RECORDMODE_ACTIVATED;
	} else {
		currentMode &= ~RECORD_MODE;

		ca->SendPMT(DEMUX_SOURCE_2, (unsigned char*) "", 0, CA_SLOT_TYPE_SMARTCARD);
		event = CZapitClient::EVT_RECORDMODE_DEACTIVATED;
	}
	SendEvent(event);
}

bool CZapit::PrepareChannels()
{
	current_channel = 0;

	if (!CServiceManager::getInstance()->LoadServices(false))
		return false;

	INFO("LoadServices: success");
	g_bouquetManager->loadBouquets();
	return true;
}

void CZapit::PrepareScan()
{
	StopPlayBack(true);
        pmt_stop_update_filter(&pmt_update_fd);
	current_channel = 0;
}

bool CZapit::StartScan(int scan_mode)
{
	PrepareScan();

	CServiceScan::getInstance()->Start(CServiceScan::SCAN_PROVIDER, (void *) scan_mode);
	return true;
}

bool CZapit::StartScanTP(TP_params * TPparams)
{
	PrepareScan();

	CServiceScan::getInstance()->Start(CServiceScan::SCAN_TRANSPONDER, (void *) TPparams);
	return true;
}

#if 0
bool CZapit::StartFastScan(int scan_mode, int opid)
{
	fast_scan_type_t scant;

	scant.type = scan_mode;
	scant.op = (fs_operator_t) opid;

	PrepareScan();

	CServiceScan::getInstance()->Start(CServiceScan::SCAN_FAST, (void *) &scant);
	return true;
}
#endif

bool CZapit::ParseCommand(CBasicMessage::Header &rmsg, int connfd)
{
	CZapitMessages::responseCmd response;
	DBG("cmd %d (version %d) received\n", rmsg.cmd, rmsg.version);

	if ((standby) && ((rmsg.cmd != CZapitMessages::CMD_SET_VOLUME)
		&& (rmsg.cmd != CZapitMessages::CMD_MUTE)
		&& (rmsg.cmd != CZapitMessages::CMD_IS_TV_CHANNEL)
		&& (rmsg.cmd != CZapitMessages::CMD_SET_STANDBY))) {
		WARN("cmd %d in standby mode", rmsg.cmd);
		//return true;
	}

	switch (rmsg.cmd) {
	case CZapitMessages::CMD_SHUTDOWN:
		return false;

	case CZapitMessages::CMD_ZAPTO:
	{
		CZapitMessages::commandZapto msgZapto;
		CBasicServer::receive_data(connfd, &msgZapto, sizeof(msgZapto)); // bouquet & channel number are already starting at 0!
		ZapTo(msgZapto.bouquet, msgZapto.channel);
		break;
	}

	case CZapitMessages::CMD_ZAPTO_CHANNELNR: {
		CZapitMessages::commandZaptoChannelNr msgZaptoChannelNr;
		CBasicServer::receive_data(connfd, &msgZaptoChannelNr, sizeof(msgZaptoChannelNr)); // bouquet & channel number are already starting at 0!
		ZapTo(msgZaptoChannelNr.channel);
		break;
	}

	case CZapitMessages::CMD_ZAPTO_SERVICEID:
	case CZapitMessages::CMD_ZAPTO_SUBSERVICEID: {
		CZapitMessages::commandZaptoServiceID msgZaptoServiceID;
		CZapitMessages::responseZapComplete msgResponseZapComplete;
		CBasicServer::receive_data(connfd, &msgZaptoServiceID, sizeof(msgZaptoServiceID));
		if(msgZaptoServiceID.record) {
			msgResponseZapComplete.zapStatus = ZapForRecord(msgZaptoServiceID.channel_id);
		} else {
			msgResponseZapComplete.zapStatus = ZapTo(msgZaptoServiceID.channel_id, (rmsg.cmd == CZapitMessages::CMD_ZAPTO_SUBSERVICEID));
		}
		CBasicServer::send_data(connfd, &msgResponseZapComplete, sizeof(msgResponseZapComplete));
		break;
	}

	case CZapitMessages::CMD_ZAPTO_SERVICEID_NOWAIT:
	case CZapitMessages::CMD_ZAPTO_SUBSERVICEID_NOWAIT: {
		CZapitMessages::commandZaptoServiceID msgZaptoServiceID;
		CBasicServer::receive_data(connfd, &msgZaptoServiceID, sizeof(msgZaptoServiceID));
		ZapTo(msgZaptoServiceID.channel_id, (rmsg.cmd == CZapitMessages::CMD_ZAPTO_SUBSERVICEID_NOWAIT));
		break;
	}

	case CZapitMessages::CMD_GET_LAST_CHANNEL: {
		CZapitClient::responseGetLastChannel lastchannel;
		lastchannel.channelNumber = (currentMode & RADIO_MODE) ? lastChannelRadio : lastChannelTV;
		lastchannel.mode = (currentMode & RADIO_MODE) ? 'r' : 't';
		CBasicServer::send_data(connfd, &lastchannel, sizeof(lastchannel)); // bouquet & channel number are already starting at 0!
		break;
	}

	case CZapitMessages::CMD_GET_CURRENT_SATELLITE_POSITION: {
		int32_t currentSatellitePosition = current_channel ? current_channel->getSatellitePosition() : CFrontend::getInstance()->getCurrentSatellitePosition();
		CBasicServer::send_data(connfd, &currentSatellitePosition, sizeof(currentSatellitePosition));
		break;
	}

	case CZapitMessages::CMD_SET_AUDIOCHAN: {
		CZapitMessages::commandSetAudioChannel msgSetAudioChannel;
		CBasicServer::receive_data(connfd, &msgSetAudioChannel, sizeof(msgSetAudioChannel));
		ChangeAudioPid(msgSetAudioChannel.channel);
		break;
	}

	case CZapitMessages::CMD_SET_MODE: {
		CZapitMessages::commandSetMode msgSetMode;
		CBasicServer::receive_data(connfd, &msgSetMode, sizeof(msgSetMode));
		if (msgSetMode.mode == CZapitClient::MODE_TV)
			SetTVMode();
		else if (msgSetMode.mode == CZapitClient::MODE_RADIO)
			SetRadioMode();
		break;
	}

	case CZapitMessages::CMD_GET_MODE: {
		CZapitMessages::responseGetMode msgGetMode;
		msgGetMode.mode = (CZapitClient::channelsMode) getMode();
		CBasicServer::send_data(connfd, &msgGetMode, sizeof(msgGetMode));
		break;
	}

	case CZapitMessages::CMD_GET_CURRENT_SERVICEID: {
		CZapitMessages::responseGetCurrentServiceID msgCurrentSID;
		msgCurrentSID.channel_id = (current_channel != 0) ? current_channel->getChannelID() : 0;
		CBasicServer::send_data(connfd, &msgCurrentSID, sizeof(msgCurrentSID));
		break;
	}

	case CZapitMessages::CMD_GET_CURRENT_SERVICEINFO: {
		CZapitClient::CCurrentServiceInfo msgCurrentServiceInfo;
		memset(&msgCurrentServiceInfo, 0, sizeof(CZapitClient::CCurrentServiceInfo));
		if(current_channel) {
			msgCurrentServiceInfo.onid = current_channel->getOriginalNetworkId();
			msgCurrentServiceInfo.sid = current_channel->getServiceId();
			msgCurrentServiceInfo.tsid = current_channel->getTransportStreamId();
			msgCurrentServiceInfo.vpid = current_channel->getVideoPid();
			msgCurrentServiceInfo.apid = current_channel->getAudioPid();
			msgCurrentServiceInfo.vtxtpid = current_channel->getTeletextPid();
			msgCurrentServiceInfo.pmtpid = current_channel->getPmtPid();
			msgCurrentServiceInfo.pmt_version = (current_channel->getCaPmt() != NULL) ? current_channel->getCaPmt()->version_number : 0xff;
			msgCurrentServiceInfo.pcrpid = current_channel->getPcrPid();
			msgCurrentServiceInfo.tsfrequency = CFrontend::getInstance()->getFrequency();
			msgCurrentServiceInfo.rate = CFrontend::getInstance()->getRate();
			msgCurrentServiceInfo.fec = CFrontend::getInstance()->getCFEC();
			msgCurrentServiceInfo.vtype = current_channel->type;
			//msgCurrentServiceInfo.diseqc = current_channel->getDiSEqC();
		}
		if(!msgCurrentServiceInfo.fec)
			msgCurrentServiceInfo.fec = (fe_code_rate)3;
		if (CFrontend::getInstance()->getInfo()->type == FE_QPSK)
			msgCurrentServiceInfo.polarisation = CFrontend::getInstance()->getPolarization();
		else
			msgCurrentServiceInfo.polarisation = 2;
		CBasicServer::send_data(connfd, &msgCurrentServiceInfo, sizeof(msgCurrentServiceInfo));
		break;
	}

	case CZapitMessages::CMD_GET_DELIVERY_SYSTEM: {
		CZapitMessages::responseDeliverySystem _response;
		switch (CFrontend::getInstance()->getInfo()->type) {
		case FE_QAM:
			_response.system = DVB_C;
			break;
		case FE_QPSK:
			_response.system = DVB_S;
			break;
		case FE_OFDM:
			_response.system = DVB_T;
			break;
		default:
			WARN("Unknown type %d", CFrontend::getInstance()->getInfo()->type);
			return false;

		}
		CBasicServer::send_data(connfd, &_response, sizeof(_response));
		break;
	}

	case CZapitMessages::CMD_GET_BOUQUETS: {
		CZapitMessages::commandGetBouquets msgGetBouquets;
		CBasicServer::receive_data(connfd, &msgGetBouquets, sizeof(msgGetBouquets));
		sendBouquets(connfd, msgGetBouquets.emptyBouquetsToo, msgGetBouquets.mode); // bouquet & channel number are already starting at 0!
		break;
	}

	case CZapitMessages::CMD_GET_BOUQUET_CHANNELS: {
		CZapitMessages::commandGetBouquetChannels msgGetBouquetChannels;
		CBasicServer::receive_data(connfd, &msgGetBouquetChannels, sizeof(msgGetBouquetChannels));
		sendBouquetChannels(connfd, msgGetBouquetChannels.bouquet, msgGetBouquetChannels.mode, false); // bouquet & channel number are already starting at 0!
		break;
	}
	case CZapitMessages::CMD_GET_BOUQUET_NCHANNELS: {
		CZapitMessages::commandGetBouquetChannels msgGetBouquetChannels;
		CBasicServer::receive_data(connfd, &msgGetBouquetChannels, sizeof(msgGetBouquetChannels));
		sendBouquetChannels(connfd, msgGetBouquetChannels.bouquet, msgGetBouquetChannels.mode, true); // bouquet & channel number are already starting at 0!
		break;
	}

	case CZapitMessages::CMD_GET_CHANNELS: {
		CZapitMessages::commandGetChannels msgGetChannels;
		CBasicServer::receive_data(connfd, &msgGetChannels, sizeof(msgGetChannels));
		sendChannels(connfd, msgGetChannels.mode, msgGetChannels.order); // bouquet & channel number are already starting at 0!
		break;
	}

        case CZapitMessages::CMD_GET_CHANNEL_NAME: {
                t_channel_id requested_channel_id;
                CZapitMessages::responseGetChannelName _response;
                CBasicServer::receive_data(connfd, &requested_channel_id, sizeof(requested_channel_id));
		_response.name[0] = 0;
		if(requested_channel_id == 0) {
			if(current_channel) {
				strncpy(_response.name, current_channel->getName().c_str(), CHANNEL_NAME_SIZE);
				_response.name[CHANNEL_NAME_SIZE-1] = 0;
			}
		} else {
			CZapitChannel * channel = CServiceManager::getInstance()->FindChannel(requested_channel_id);
			if(channel) {
				strncpy(_response.name, channel->getName().c_str(), CHANNEL_NAME_SIZE);
				_response.name[CHANNEL_NAME_SIZE-1] = 0;
			}
		}
                CBasicServer::send_data(connfd, &_response, sizeof(_response));
                break;
        }

       case CZapitMessages::CMD_IS_TV_CHANNEL: {
               t_channel_id                             requested_channel_id;
               CZapitMessages::responseGeneralTrueFalse _response;
               CBasicServer::receive_data(connfd, &requested_channel_id, sizeof(requested_channel_id));

		/* if in doubt (i.e. unknown channel) answer yes */
		_response.status = true;
		CZapitChannel * channel = CServiceManager::getInstance()->FindChannel(requested_channel_id);
		if(channel)
			/* FIXME: the following check is no even remotely accurate */
			_response.status = (channel->getServiceType() != ST_DIGITAL_RADIO_SOUND_SERVICE);
		
		CBasicServer::send_data(connfd, &_response, sizeof(_response));
		break;
        }

	case CZapitMessages::CMD_BQ_RESTORE: {
		//2004.08.02 g_bouquetManager->restoreBouquets();
		if(list_changed) {
			PrepareChannels();
			list_changed = 0;
		} else {
			g_bouquetManager->clearAll();
			g_bouquetManager->loadBouquets();
		}
		response.cmd = CZapitMessages::CMD_READY;
		CBasicServer::send_data(connfd, &response, sizeof(response));
		break;
	}

	case CZapitMessages::CMD_REINIT_CHANNELS: {
		// Houdini: save actual channel to restore it later, old version's channel was set to scans.conf initial channel
  		t_channel_id cid= current_channel ? current_channel->getChannelID() : 0;

   		PrepareChannels();

		current_channel = CServiceManager::getInstance()->FindChannel(cid);

		response.cmd = CZapitMessages::CMD_READY;
		CBasicServer::send_data(connfd, &response, sizeof(response));
		SendEvent(CZapitClient::EVT_SERVICES_CHANGED);
		break;
	}

	case CZapitMessages::CMD_RELOAD_CURRENTSERVICES: {
#if 0
  	        response.cmd = CZapitMessages::CMD_READY;
  	        CBasicServer::send_data(connfd, &response, sizeof(response));
#endif
		DBG("[zapit] sending EVT_SERVICES_CHANGED\n");
		CFrontend::getInstance()->setTsidOnid(0);
		ZapIt(live_channel_id, current_is_nvod);
  	        SendEvent(CZapitClient::EVT_SERVICES_CHANGED);
		//SendEvent(CZapitClient::EVT_BOUQUETS_CHANGED);
  	        break;
  	}
	case CZapitMessages::CMD_SCANSTART: {
		int scan_mode;
		CBasicServer::receive_data(connfd, &scan_mode, sizeof(scan_mode));

		if (!StartScan(scan_mode))
			SendEvent(CZapitClient::EVT_SCAN_FAILED);
		break;
	}
	case CZapitMessages::CMD_SCANSTOP:
		CServiceScan::getInstance()->Abort();
		CServiceScan::getInstance()->Stop();
		break;

	case CZapitMessages::CMD_SETCONFIG:
		Zapit_config Cfg;
                CBasicServer::receive_data(connfd, &Cfg, sizeof(Cfg));
		SetConfig(&Cfg);
		break;
	case CZapitMessages::CMD_GETCONFIG:
		SendConfig(connfd);
		break;
	case CZapitMessages::CMD_REZAP:
		if (currentMode & RECORD_MODE)
			break;
		if(config.rezapTimeout > 0)
			sleep(config.rezapTimeout);
		if(current_channel)
			ZapIt(current_channel->getChannelID());
		break;
        case CZapitMessages::CMD_TUNE_TP: {
			CBasicServer::receive_data(connfd, &TP, sizeof(TP));
			sig_delay = 0;
			TP.feparams.inversion = INVERSION_AUTO;
			const char *name = scanProviders.size() > 0  ? scanProviders.begin()->second.c_str() : "unknown";

			switch (CFrontend::getInstance()->getInfo()->type) {
			case FE_QPSK:
			case FE_OFDM: {
				t_satellite_position satellitePosition = scanProviders.begin()->first;
				printf("[zapit] tune to sat %s freq %d rate %d fec %d pol %d\n", name, TP.feparams.frequency, TP.feparams.u.qpsk.symbol_rate, TP.feparams.u.qpsk.fec_inner, TP.polarization);
				CFrontend::getInstance()->setInput(satellitePosition, TP.feparams.frequency,  TP.polarization);
				CFrontend::getInstance()->driveToSatellitePosition(satellitePosition);
				break;
			}
			case FE_QAM:
				printf("[zapit] tune to cable %s freq %d rate %d fec %d\n", name, TP.feparams.frequency, TP.feparams.u.qam.symbol_rate, TP.feparams.u.qam.fec_inner);
				break;
			default:
				WARN("Unknown type %d", CFrontend::getInstance()->getInfo()->type);
				return false;
			}
			CFrontend::getInstance()->tuneFrequency(&TP.feparams, TP.polarization, true);
		}
		break;
        case CZapitMessages::CMD_SCAN_TP: {
                CBasicServer::receive_data(connfd, &TP, sizeof(TP));

#if 0
printf("[zapit] TP_id %d freq %d rate %d fec %d pol %d\n", TP.TP_id, TP.feparams.frequency, TP.feparams.u.qpsk.symbol_rate, TP.feparams.u.qpsk.fec_inner, TP.polarization);
#endif
		if(!(TP.feparams.frequency > 0) && current_channel) {
			transponder_list_t::iterator transponder = transponders.find(current_channel->getTransponderId());
			TP.feparams.frequency = transponder->second.feparams.frequency;
			switch (CFrontend::getInstance()->getInfo()->type) {
			case FE_QPSK:
			case FE_OFDM:
				TP.feparams.u.qpsk.symbol_rate = transponder->second.feparams.u.qpsk.symbol_rate;
				TP.feparams.u.qpsk.fec_inner = transponder->second.feparams.u.qpsk.fec_inner;
				TP.polarization = transponder->second.polarization;
				break;
			case FE_QAM:
				TP.feparams.u.qam.symbol_rate = transponder->second.feparams.u.qam.symbol_rate;
				TP.feparams.u.qam.fec_inner = transponder->second.feparams.u.qam.fec_inner;
				TP.feparams.u.qam.modulation = transponder->second.feparams.u.qam.modulation;
				break;
			default:
				WARN("Unknown type %d", CFrontend::getInstance()->getInfo()->type);
				return false;
			}

			if(scanProviders.size() > 0)
				scanProviders.clear();
#if 0
			std::map<string, t_satellite_position>::iterator spos_it;
			for (spos_it = satellitePositions.begin(); spos_it != satellitePositions.end(); spos_it++)
				if(spos_it->second == current_channel->getSatellitePosition())
					scanProviders[transponder->second.DiSEqC] = spos_it->first.c_str();
#endif
			//FIXME not ready
			//if(satellitePositions.find(current_channel->getSatellitePosition()) != satellitePositions.end())
			current_channel = 0;
		}
#if 0
		PrepareScan();
		CServiceScan::getInstance()->Start(CServiceScan::SCAN_TRANSPONDER, (void *) &TP);
#endif
		StartScanTP(&TP);
                break;
        }

	case CZapitMessages::CMD_SCANREADY: {
		CZapitMessages::responseIsScanReady msgResponseIsScanReady;
#if 0 //FIXME used only when scanning done using pzapit client, is it really needed ?
		msgResponseIsScanReady.satellite = curr_sat;
		msgResponseIsScanReady.transponder = found_transponders;
		msgResponseIsScanReady.processed_transponder = processed_transponders;
		msgResponseIsScanReady.services = found_channels;
		msgResponseIsScanReady.scanReady = !CServiceScan::getInstance()->Scanning();
#endif
		CBasicServer::send_data(connfd, &msgResponseIsScanReady, sizeof(msgResponseIsScanReady));
		break;
	}

	case CZapitMessages::CMD_SCANGETSATLIST: {
		uint32_t  satlength;
		CZapitClient::responseGetSatelliteList sat;
		satlength = sizeof(sat);

		sat_iterator_t sit;
		for(sit = satellitePositions.begin(); sit != satellitePositions.end(); sit++) {
			strncpy(sat.satName, sit->second.name.c_str(), 50);
			sat.satName[49] = 0;
			sat.satPosition = sit->first;
			sat.motorPosition = sit->second.motor_position;
			CBasicServer::send_data(connfd, &satlength, sizeof(satlength));
			CBasicServer::send_data(connfd, (char *)&sat, satlength);
		}
		satlength = SATNAMES_END_MARKER;
		CBasicServer::send_data(connfd, &satlength, sizeof(satlength));
		break;
	}

	case CZapitMessages::CMD_SCANSETSCANSATLIST: {
		CZapitClient::commandSetScanSatelliteList sat;
		scanProviders.clear();
		while (CBasicServer::receive_data(connfd, &sat, sizeof(sat))) {
			printf("[zapit] adding to scan %s (position %d)\n", sat.satName, sat.position);
			scanProviders[sat.position] = sat.satName;
		}
		break;
	}

	case CZapitMessages::CMD_SCANSETSCANMOTORPOSLIST: {
		// absolute
		break;
	}

	case CZapitMessages::CMD_SCANSETDISEQCTYPE: {
		//diseqcType is global
		CBasicServer::receive_data(connfd, &diseqcType, sizeof(diseqcType));
		CFrontend::getInstance()->setDiseqcType(diseqcType);
		DBG("set diseqc type %d", diseqcType);
		break;
	}

	case CZapitMessages::CMD_SCANSETDISEQCREPEAT: {
		uint32_t  repeats;
		CBasicServer::receive_data(connfd, &repeats, sizeof(repeats));
		CFrontend::getInstance()->setDiseqcRepeats(repeats);
		DBG("set diseqc repeats to %d", repeats);
		break;
	}

	case CZapitMessages::CMD_SCANSETBOUQUETMODE:
		CBasicServer::receive_data(connfd, &bouquetMode, sizeof(bouquetMode));
		break;

        case CZapitMessages::CMD_SCANSETTYPE:
                CBasicServer::receive_data(connfd, &scanType, sizeof(scanType));
                break;

	case CZapitMessages::CMD_SET_EVENT_MODE: {
		CZapitMessages::commandSetRecordMode msgSetRecordMode;
		CBasicServer::receive_data(connfd, &msgSetRecordMode, sizeof(msgSetRecordMode));
		//printf("[zapit] event mode: %d\n", msgSetRecordMode.activate);fflush(stdout);
		event_mode = msgSetRecordMode.activate;
                break;
	}
	case CZapitMessages::CMD_SET_RECORD_MODE: {
		CZapitMessages::commandSetRecordMode msgSetRecordMode;
		CBasicServer::receive_data(connfd, &msgSetRecordMode, sizeof(msgSetRecordMode));
		printf("[zapit] recording mode: %d\n", msgSetRecordMode.activate);fflush(stdout);
		SetRecordMode(msgSetRecordMode.activate);
		break;
	}

	case CZapitMessages::CMD_GET_RECORD_MODE: {
		CZapitMessages::responseGetRecordModeState msgGetRecordModeState;
		msgGetRecordModeState.activated = (currentMode & RECORD_MODE);
		CBasicServer::send_data(connfd, &msgGetRecordModeState, sizeof(msgGetRecordModeState));
		break;
	}

	case CZapitMessages::CMD_SB_GET_PLAYBACK_ACTIVE: {
		CZapitMessages::responseGetPlaybackState msgGetPlaybackState;
                msgGetPlaybackState.activated = playing;
#if 0 //FIXME
		if (videoDecoder) {
                	if (videoDecoder->getPlayState() == VIDEO_PLAYING)
                        	msgGetPlaybackState.activated = 1;
                }
#endif
		CBasicServer::send_data(connfd, &msgGetPlaybackState, sizeof(msgGetPlaybackState));
		break;
	}

	case CZapitMessages::CMD_BQ_ADD_BOUQUET: {
		char * name = CBasicServer::receive_string(connfd);
		g_bouquetManager->addBouquet(name, true);
		CBasicServer::delete_string(name);
		break;
	}

	case CZapitMessages::CMD_BQ_DELETE_BOUQUET: {
		CZapitMessages::commandDeleteBouquet msgDeleteBouquet;
		CBasicServer::receive_data(connfd, &msgDeleteBouquet, sizeof(msgDeleteBouquet)); // bouquet & channel number are already starting at 0!
		g_bouquetManager->deleteBouquet(msgDeleteBouquet.bouquet);
		break;
	}

	case CZapitMessages::CMD_BQ_RENAME_BOUQUET: {
		CZapitMessages::commandRenameBouquet msgRenameBouquet;
		CBasicServer::receive_data(connfd, &msgRenameBouquet, sizeof(msgRenameBouquet)); // bouquet & channel number are already starting at 0!
		char * name = CBasicServer::receive_string(connfd);
		if (msgRenameBouquet.bouquet < g_bouquetManager->Bouquets.size()) {
			g_bouquetManager->Bouquets[msgRenameBouquet.bouquet]->Name = name;
			g_bouquetManager->Bouquets[msgRenameBouquet.bouquet]->bUser = true;
		}
		CBasicServer::delete_string(name);
		break;
	}

	case CZapitMessages::CMD_BQ_EXISTS_BOUQUET: {
		CZapitMessages::responseGeneralInteger responseInteger;

		char * name = CBasicServer::receive_string(connfd);
		responseInteger.number = g_bouquetManager->existsBouquet(name);
		CBasicServer::delete_string(name);
		CBasicServer::send_data(connfd, &responseInteger, sizeof(responseInteger)); // bouquet & channel number are already starting at 0!
		break;
	}

	case CZapitMessages::CMD_BQ_EXISTS_CHANNEL_IN_BOUQUET: {
		CZapitMessages::commandExistsChannelInBouquet msgExistsChInBq;
		CZapitMessages::responseGeneralTrueFalse responseBool;
		CBasicServer::receive_data(connfd, &msgExistsChInBq, sizeof(msgExistsChInBq)); // bouquet & channel number are already starting at 0!
		responseBool.status = g_bouquetManager->existsChannelInBouquet(msgExistsChInBq.bouquet, msgExistsChInBq.channel_id);
		CBasicServer::send_data(connfd, &responseBool, sizeof(responseBool));
		break;
	}

	case CZapitMessages::CMD_BQ_MOVE_BOUQUET: {
		CZapitMessages::commandMoveBouquet msgMoveBouquet;
		CBasicServer::receive_data(connfd, &msgMoveBouquet, sizeof(msgMoveBouquet)); // bouquet & channel number are already starting at 0!
		g_bouquetManager->moveBouquet(msgMoveBouquet.bouquet, msgMoveBouquet.newPos);
		break;
	}

	case CZapitMessages::CMD_BQ_ADD_CHANNEL_TO_BOUQUET: {
		CZapitMessages::commandAddChannelToBouquet msgAddChannelToBouquet;
		CBasicServer::receive_data(connfd, &msgAddChannelToBouquet, sizeof(msgAddChannelToBouquet)); // bouquet & channel number are already starting at 0!
		addChannelToBouquet(msgAddChannelToBouquet.bouquet, msgAddChannelToBouquet.channel_id);
		break;
	}

	case CZapitMessages::CMD_BQ_REMOVE_CHANNEL_FROM_BOUQUET: {
		CZapitMessages::commandRemoveChannelFromBouquet msgRemoveChannelFromBouquet;
		CBasicServer::receive_data(connfd, &msgRemoveChannelFromBouquet, sizeof(msgRemoveChannelFromBouquet)); // bouquet & channel number are already starting at 0!
		if (msgRemoveChannelFromBouquet.bouquet < g_bouquetManager->Bouquets.size())
			g_bouquetManager->Bouquets[msgRemoveChannelFromBouquet.bouquet]->removeService(msgRemoveChannelFromBouquet.channel_id);
#if 1 // REAL_REMOVE
		bool status = 0;
		for (unsigned int i = 0; i < g_bouquetManager->Bouquets.size(); i++) {
			status = g_bouquetManager->existsChannelInBouquet(i, msgRemoveChannelFromBouquet.channel_id);
			if(status)
				break;
		}
		if(!status) {
			CServiceManager::getInstance()->RemoveChannel(msgRemoveChannelFromBouquet.channel_id);
			current_channel = 0;
			list_changed = 1;
		}
#endif
		break;
	}

	case CZapitMessages::CMD_BQ_MOVE_CHANNEL: {
		CZapitMessages::commandMoveChannel msgMoveChannel;
		CBasicServer::receive_data(connfd, &msgMoveChannel, sizeof(msgMoveChannel)); // bouquet & channel number are already starting at 0!
		if (msgMoveChannel.bouquet < g_bouquetManager->Bouquets.size())
			g_bouquetManager->Bouquets[msgMoveChannel.bouquet]->moveService(msgMoveChannel.oldPos, msgMoveChannel.newPos,
					(((currentMode & RADIO_MODE) && msgMoveChannel.mode == CZapitClient::MODE_CURRENT)
					 || (msgMoveChannel.mode==CZapitClient::MODE_RADIO)) ? 2 : 1);
		break;
	}

	case CZapitMessages::CMD_BQ_SET_LOCKSTATE: {
		CZapitMessages::commandBouquetState msgBouquetLockState;
		CBasicServer::receive_data(connfd, &msgBouquetLockState, sizeof(msgBouquetLockState)); // bouquet & channel number are already starting at 0!
		if (msgBouquetLockState.bouquet < g_bouquetManager->Bouquets.size())
			g_bouquetManager->Bouquets[msgBouquetLockState.bouquet]->bLocked = msgBouquetLockState.state;
		break;
	}

	case CZapitMessages::CMD_BQ_SET_HIDDENSTATE: {
		CZapitMessages::commandBouquetState msgBouquetHiddenState;
		CBasicServer::receive_data(connfd, &msgBouquetHiddenState, sizeof(msgBouquetHiddenState)); // bouquet & channel number are already starting at 0!
		if (msgBouquetHiddenState.bouquet < g_bouquetManager->Bouquets.size())
			g_bouquetManager->Bouquets[msgBouquetHiddenState.bouquet]->bHidden = msgBouquetHiddenState.state;
		break;
	}

	case CZapitMessages::CMD_BQ_RENUM_CHANNELLIST:
		g_bouquetManager->renumServices();
		// 2004.08.02 g_bouquetManager->storeBouquets();
		break;

	case CZapitMessages::CMD_BQ_SAVE_BOUQUETS: {
		CZapitMessages::commandBoolean msgBoolean;
		CBasicServer::receive_data(connfd, &msgBoolean, sizeof(msgBoolean));

		response.cmd = CZapitMessages::CMD_READY;
		CBasicServer::send_data(connfd, &response, sizeof(response));
#if 0
		//if (msgBoolean.truefalse)
		if(list_changed) {
			SendEvent(CZapitClient::EVT_SERVICES_CHANGED);
		} else
			SendEvent(CZapitClient::EVT_BOUQUETS_CHANGED);
#endif
		g_bouquetManager->saveBouquets();
		g_bouquetManager->saveUBouquets();
		g_bouquetManager->renumServices();
		//SendEvent(CZapitClient::EVT_SERVICES_CHANGED);
		SendEvent(CZapitClient::EVT_BOUQUETS_CHANGED);
		if(list_changed) {
			CServiceManager::getInstance()->SaveServices(true);
			list_changed = 0;
		}
		break;
	}

        case CZapitMessages::CMD_SET_VIDEO_SYSTEM: {
		CZapitMessages::commandInt msg;
		CBasicServer::receive_data(connfd, &msg, sizeof(msg));
		videoDecoder->SetVideoSystem(msg.val);
                break;
        }

        case CZapitMessages::CMD_SET_NTSC: {
		videoDecoder->SetVideoSystem(8);
                break;
        }

	case CZapitMessages::CMD_SB_START_PLAYBACK:
		//playbackStopForced = false;
		StartPlayBack(current_channel);
		break;

	case CZapitMessages::CMD_SB_STOP_PLAYBACK:
		StopPlayBack(false);
		response.cmd = CZapitMessages::CMD_READY;
		CBasicServer::send_data(connfd, &response, sizeof(response));
		break;

	case CZapitMessages::CMD_SB_LOCK_PLAYBACK:
		/* hack. if standby true, dont blank video */
		standby = true;
		StopPlayBack(true);
		standby = false;
		playbackStopForced = true;
		response.cmd = CZapitMessages::CMD_READY;
		CBasicServer::send_data(connfd, &response, sizeof(response));
		break;
	case CZapitMessages::CMD_SB_UNLOCK_PLAYBACK:
		playbackStopForced = false;
		StartPlayBack(current_channel);
		SendPMT();
		response.cmd = CZapitMessages::CMD_READY;
		CBasicServer::send_data(connfd, &response, sizeof(response));
		break;
	case CZapitMessages::CMD_SET_DISPLAY_FORMAT: {
		CZapitMessages::commandInt msg;
		CBasicServer::receive_data(connfd, &msg, sizeof(msg));
#if 0 //FIXME
		videoDecoder->setCroppingMode((video_displayformat_t) msg.val);
#endif
		break;
	}

	case CZapitMessages::CMD_SET_AUDIO_MODE: {
		CZapitMessages::commandInt msg;
		CBasicServer::receive_data(connfd, &msg, sizeof(msg));
		audioDecoder->setChannel((int) msg.val);
		audio_mode = msg.val;
		break;
	}

	case CZapitMessages::CMD_GET_AUDIO_MODE: {
		CZapitMessages::commandInt msg;
		msg.val = (int) audio_mode;
		CBasicServer::send_data(connfd, &msg, sizeof(msg));
		break;
	}

	case CZapitMessages::CMD_SET_ASPECTRATIO: {
		CZapitMessages::commandInt msg;
		CBasicServer::receive_data(connfd, &msg, sizeof(msg));
		aspectratio=(int) msg.val;
		videoDecoder->setAspectRatio(aspectratio, -1);
		break;
	}

	case CZapitMessages::CMD_GET_ASPECTRATIO: {
		CZapitMessages::commandInt msg;
		aspectratio=videoDecoder->getAspectRatio();
		msg.val = aspectratio;
		CBasicServer::send_data(connfd, &msg, sizeof(msg));
		break;
	}

	case CZapitMessages::CMD_SET_MODE43: {
		CZapitMessages::commandInt msg;
		CBasicServer::receive_data(connfd, &msg, sizeof(msg));
		mode43=(int) msg.val;
		videoDecoder->setAspectRatio(-1, mode43);
		break;
	}

#if 0 
	//FIXME howto read aspect mode back?
	case CZapitMessages::CMD_GET_MODE43: {
		CZapitMessages::commandInt msg;
		mode43=videoDecoder->getCroppingMode();
		msg.val = mode43;
		CBasicServer::send_data(connfd, &msg, sizeof(msg));
		break;
	}
#endif

	case CZapitMessages::CMD_GETPIDS: {
		if (current_channel) {
			CZapitClient::responseGetOtherPIDs responseGetOtherPIDs;
			responseGetOtherPIDs.vpid = current_channel->getVideoPid();
			responseGetOtherPIDs.ecmpid = 0; // TODO: remove
			responseGetOtherPIDs.vtxtpid = current_channel->getTeletextPid();
			responseGetOtherPIDs.pmtpid = current_channel->getPmtPid();
			responseGetOtherPIDs.pcrpid = current_channel->getPcrPid();
			responseGetOtherPIDs.selected_apid = current_channel->getAudioChannelIndex();
			responseGetOtherPIDs.privatepid = current_channel->getPrivatePid();
			CBasicServer::send_data(connfd, &responseGetOtherPIDs, sizeof(responseGetOtherPIDs));
			sendAPIDs(connfd);
		}
		break;
	}

	case CZapitMessages::CMD_GET_FE_SIGNAL: {
		CZapitClient::responseFESignal response_FEsig;

		response_FEsig.sig = CFrontend::getInstance()->getSignalStrength();
		response_FEsig.snr = CFrontend::getInstance()->getSignalNoiseRatio();
		response_FEsig.ber = CFrontend::getInstance()->getBitErrorRate();

		CBasicServer::send_data(connfd, &response_FEsig, sizeof(CZapitClient::responseFESignal));
		break;
	}

	case CZapitMessages::CMD_SETSUBSERVICES: {
		CZapitClient::commandAddSubServices msgAddSubService;

		t_satellite_position satellitePosition = current_channel ? current_channel->getSatellitePosition() : 0;
		while (CBasicServer::receive_data(connfd, &msgAddSubService, sizeof(msgAddSubService))) {
#if 0
			t_original_network_id original_network_id = msgAddSubService.original_network_id;
			t_service_id          service_id          = msgAddSubService.service_id;
			t_channel_id sub_channel_id = 
				((uint64_t) ( satellitePosition >= 0 ? satellitePosition : (uint64_t)(0xF000+ abs(satellitePosition))) << 48) |
				(uint64_t) CREATE_CHANNEL_ID_FROM_SERVICE_ORIGINALNETWORK_TRANSPORTSTREAM_ID(msgAddSubService.service_id, msgAddSubService.original_network_id, msgAddSubService.transport_stream_id);
			DBG("NVOD insert %llx\n", sub_channel_id);
			nvodchannels.insert (
					std::pair <t_channel_id, CZapitChannel> (
						sub_channel_id,
						CZapitChannel (
							"NVOD",
							service_id,
							msgAddSubService.transport_stream_id,
							original_network_id,
							1,
							satellitePosition,
							0
							)
						)
					);
#endif
			CZapitChannel * channel = new CZapitChannel (
					"NVOD",
					msgAddSubService.service_id,
					msgAddSubService.transport_stream_id,
					msgAddSubService.original_network_id,
					ST_DIGITAL_TELEVISION_SERVICE,
					satellitePosition,
					0
					);
			CServiceManager::getInstance()->AddNVODChannel(channel);
		}

		current_is_nvod = true;
		break;
	}

	case CZapitMessages::CMD_REGISTEREVENTS:
		eventServer->registerEvent(connfd);
		break;

	case CZapitMessages::CMD_UNREGISTEREVENTS:
		eventServer->unRegisterEvent(connfd);
		break;

	case CZapitMessages::CMD_MUTE: {
		CZapitMessages::commandBoolean msgBoolean;
		CBasicServer::receive_data(connfd, &msgBoolean, sizeof(msgBoolean));
		//printf("[zapit] mute %d\n", msgBoolean.truefalse);
		if (msgBoolean.truefalse)
			audioDecoder->mute();
		else
			audioDecoder->unmute();
		break;
	}

	case CZapitMessages::CMD_SET_VOLUME: {
		CZapitMessages::commandVolume msgVolume;
		CBasicServer::receive_data(connfd, &msgVolume, sizeof(msgVolume));
		audioDecoder->setVolume(msgVolume.left, msgVolume.right);
                volume_left = msgVolume.left;
                volume_right = msgVolume.right;
		break;
	}
        case CZapitMessages::CMD_GET_VOLUME: {
                CZapitMessages::commandVolume msgVolume;
                msgVolume.left = volume_left;
                msgVolume.right = volume_right;
                CBasicServer::send_data(connfd, &msgVolume, sizeof(msgVolume));
                break;
        }
#ifdef EVOLUX
	case CZapitMessages::CMD_SET_VOLUME_PERCENT: {
		CZapitMessages::commandVolumePercent msgVolumePercent;
		CBasicServer::receive_data(connfd, &msgVolumePercent, sizeof(msgVolumePercent));
		if (!msgVolumePercent.apid)
			msgVolumePercent.apid = current_channel->getAudioPid();
		volume_map[make_pair(live_channel_id, msgVolumePercent.apid)] = msgVolumePercent.percent;
		break;
	}
	case CZapitMessages::CMD_GET_VOLUME_PERCENT: {
		CZapitMessages::commandVolumePercent msgVolumePercent;
		CBasicServer::receive_data(connfd, &msgVolumePercent, sizeof(msgVolumePercent));
		if (!msgVolumePercent.apid)
			msgVolumePercent.apid = current_channel->getAudioPid();
		t_chan_apid chan_apid = make_pair(live_channel_id, msgVolumePercent.apid);
		volume_map_it = volume_map.find(chan_apid);
		if (volume_map_it != volume_map.end())
			msgVolumePercent.percent = volume_map[chan_apid];
		else for (int  i = 0; i < current_channel->getAudioChannelCount(); i++)
				if (msgVolumePercent.apid == current_channel->getAudioPid(i)) {
					msgVolumePercent.percent =
						(current_channel->getAudioChannel(i)->audioChannelType == CZapitAudioChannel::AC3)
						? VOLUME_DEFAULT_AC3
						: VOLUME_DEFAULT_PCM;
					break;
				}
		CBasicServer::send_data(connfd, &msgVolumePercent, sizeof(msgVolumePercent));
		break;
	}
#endif
	case CZapitMessages::CMD_GET_MUTE_STATUS: {
		CZapitMessages::commandBoolean msgBoolean;
		msgBoolean.truefalse = audioDecoder->getMuteStatus();
		CBasicServer::send_data(connfd, &msgBoolean, sizeof(msgBoolean));
		break;
	}
	case CZapitMessages::CMD_SET_STANDBY: {
		CZapitMessages::commandBoolean msgBoolean;
		CBasicServer::receive_data(connfd, &msgBoolean, sizeof(msgBoolean));
		if (msgBoolean.truefalse) {
			// if(currentMode & RECORD_MODE) videoDecoder->freeze();
			enterStandby();
		} else
			leaveStandby();
		response.cmd = CZapitMessages::CMD_READY;
		CBasicServer::send_data(connfd, &response, sizeof(response));
		break;
	}

	case CZapitMessages::CMD_NVOD_SUBSERVICE_NUM: {
		CZapitMessages::commandInt msg;
		CBasicServer::receive_data(connfd, &msg, sizeof(msg));
	}

	case CZapitMessages::CMD_SEND_MOTOR_COMMAND: {
		CZapitMessages::commandMotor msgMotor;
		CBasicServer::receive_data(connfd, &msgMotor, sizeof(msgMotor));
		printf("[zapit] received motor command: %x %x %x %x %x %x\n", msgMotor.cmdtype, msgMotor.address, msgMotor.cmd, msgMotor.num_parameters, msgMotor.param1, msgMotor.param2);
		if(msgMotor.cmdtype > 0x20)
		CFrontend::getInstance()->sendMotorCommand(msgMotor.cmdtype, msgMotor.address, msgMotor.cmd, msgMotor.num_parameters, msgMotor.param1, msgMotor.param2);
		// TODO !!
		//else if(current_channel)
		//  CFrontend::getInstance()->satFind(msgMotor.cmdtype, current_channel);
		break;
	}

	default:
		WARN("unknown command %d (version %d)", rmsg.cmd, CZapitMessages::ACTVERSION);
		break;
	}

	DBG("cmd %d processed\n", rmsg.cmd);

	return true;
}

void CZapit::addChannelToBouquet(const unsigned int bouquet, const t_channel_id channel_id)
{
	//DBG("addChannelToBouquet(%d, %d)\n", bouquet, channel_id);

	CZapitChannel* chan = CServiceManager::getInstance()->FindChannel(channel_id);

	if (chan != NULL)
		if (bouquet < g_bouquetManager->Bouquets.size())
			g_bouquetManager->Bouquets[bouquet]->addService(chan);
		else
			WARN("bouquet not found");
	else
		WARN("channel_id not found in channellist");
}

bool CZapit::send_data_count(int connfd, int data_count)
{
	CZapitMessages::responseGeneralInteger responseInteger;
	responseInteger.number = data_count;
	if (CBasicServer::send_data(connfd, &responseInteger, sizeof(responseInteger)) == false) {
		ERROR("could not send any return");
		return false;
	}
	return true;
}

void CZapit::sendAPIDs(int connfd)
{
	if (!send_data_count(connfd, current_channel->getAudioChannelCount()))
		return;
	for (uint32_t  i = 0; i < current_channel->getAudioChannelCount(); i++) {
		CZapitClient::responseGetAPIDs response;
		response.pid = current_channel->getAudioPid(i);
		strncpy(response.desc, current_channel->getAudioChannel(i)->description.c_str(), DESC_MAX_LEN);
		response.is_ac3 = response.is_aac = 0;
		if (current_channel->getAudioChannel(i)->audioChannelType == CZapitAudioChannel::AC3) {
			response.is_ac3 = 1;
		} else if (current_channel->getAudioChannel(i)->audioChannelType == CZapitAudioChannel::AAC) {
			response.is_aac = 1;
		}
		response.component_tag = current_channel->getAudioChannel(i)->componentTag;

		if (CBasicServer::send_data(connfd, &response, sizeof(response)) == false) {
			ERROR("could not send any return");
			return;
		}
	}
}

void CZapit::internalSendChannels(int connfd, ZapitChannelList* channels, const unsigned int first_channel_nr, bool nonames)
{
	int data_count = channels->size();
#if RECORD_RESEND // old, before tv/radio resend
	if (currentMode & RECORD_MODE) {
		for (uint32_t  i = 0; i < channels->size(); i++)
			if ((*channels)[i]->getTransponderId() != channel->getTransponderId())
				data_count--;
	}
#endif
	if (!send_data_count(connfd, data_count))
		return;

	for (uint32_t  i = 0; i < channels->size();i++) {
#if RECORD_RESEND // old, before tv/radio resend
		if ((currentMode & RECORD_MODE) && ((*channels)[i]->getTransponderId() != CFrontend::getInstance()->getTsidOnid()))
			continue;
#endif

		if(nonames) {
			CZapitClient::responseGetBouquetNChannels response;
			response.nr = first_channel_nr + i;

			if (CBasicServer::send_data(connfd, &response, sizeof(response)) == false) {
				ERROR("could not send any return");
				if (CBasicServer::send_data(connfd, &response, sizeof(response)) == false) {
					ERROR("could not send any return, stop");
					return;
				}
			}
		} else {
			CZapitClient::responseGetBouquetChannels response;
			strncpy(response.name, ((*channels)[i]->getName()).c_str(), CHANNEL_NAME_SIZE);
			response.name[CHANNEL_NAME_SIZE-1] = 0;
//printf("internalSendChannels: name %s\n", response.name);
			response.satellitePosition = (*channels)[i]->getSatellitePosition();
			response.channel_id = (*channels)[i]->getChannelID();
			response.nr = first_channel_nr + i;

			if (CBasicServer::send_data(connfd, &response, sizeof(response)) == false) {
				ERROR("could not send any return");
DBG("current: %d name %s total %d\n", i, response.name, data_count);
				if (CBasicServer::send_data(connfd, &response, sizeof(response)) == false) {
					ERROR("could not send any return, stop");
					return;
				}
			}
		}
	}
}

void CZapit::sendBouquets(int connfd, const bool emptyBouquetsToo, CZapitClient::channelsMode mode)
{
	CZapitClient::responseGetBouquets msgBouquet;
        int curMode;
        switch(mode) {
                case CZapitClient::MODE_TV:
                        curMode = TV_MODE;
                        break;
                case CZapitClient::MODE_RADIO:
                        curMode = RADIO_MODE;
                        break;
                case CZapitClient::MODE_CURRENT:
                default:
                        curMode = currentMode;
                        break;
        }

        for (uint32_t i = 0; i < g_bouquetManager->Bouquets.size(); i++) {
                if (emptyBouquetsToo || (!g_bouquetManager->Bouquets[i]->bHidden && g_bouquetManager->Bouquets[i]->bUser)
                    || ((!g_bouquetManager->Bouquets[i]->bHidden)
                     && (((curMode & RADIO_MODE) && !g_bouquetManager->Bouquets[i]->radioChannels.empty()) ||
                      ((curMode & TV_MODE) && !g_bouquetManager->Bouquets[i]->tvChannels.empty())))
                   )
		{
// ATTENTION: in RECORD_MODE empty bouquets are not send!
#if RECORD_RESEND // old, before tv/radio resend
			if ((!(currentMode & RECORD_MODE)) || ((CFrontend::getInstance() != NULL) &&
			     (((currentMode & RADIO_MODE) && (g_bouquetManager->Bouquets[i]->recModeRadioSize(CFrontend::getInstance()->getTsidOnid()) > 0)) ||
			      ((currentMode & TV_MODE)    && (g_bouquetManager->Bouquets[i]->recModeTVSize   (CFrontend::getInstance()->getTsidOnid()) > 0)))))
#endif
			{
				msgBouquet.bouquet_nr = i;
				strncpy(msgBouquet.name, g_bouquetManager->Bouquets[i]->Name.c_str(), 30);
				msgBouquet.name[29] = 0;
				msgBouquet.locked     = g_bouquetManager->Bouquets[i]->bLocked;
				msgBouquet.hidden     = g_bouquetManager->Bouquets[i]->bHidden;
				if (CBasicServer::send_data(connfd, &msgBouquet, sizeof(msgBouquet)) == false) {
					ERROR("could not send any return");
					return;
				}
			}
		}
	}
	msgBouquet.bouquet_nr = RESPONSE_GET_BOUQUETS_END_MARKER;
	if (CBasicServer::send_data(connfd, &msgBouquet, sizeof(msgBouquet)) == false) {
		ERROR("could not send end marker");
		return;
	}
}

void CZapit::sendBouquetChannels(int connfd, const unsigned int bouquet, const CZapitClient::channelsMode mode, bool nonames)
{
	if (bouquet >= g_bouquetManager->Bouquets.size()) {
		WARN("invalid bouquet number: %d", bouquet);
		return;
	}

	if (((currentMode & RADIO_MODE) && (mode == CZapitClient::MODE_CURRENT)) || (mode == CZapitClient::MODE_RADIO))
		internalSendChannels(connfd, &(g_bouquetManager->Bouquets[bouquet]->radioChannels), g_bouquetManager->radioChannelsBegin().getNrofFirstChannelofBouquet(bouquet), nonames);
	else
		internalSendChannels(connfd, &(g_bouquetManager->Bouquets[bouquet]->tvChannels), g_bouquetManager->tvChannelsBegin().getNrofFirstChannelofBouquet(bouquet), nonames);
}

void CZapit::sendChannels(int connfd, const CZapitClient::channelsMode mode, const CZapitClient::channelsOrder order)
{
	ZapitChannelList channels;

	if (order == CZapitClient::SORT_BOUQUET) {
		CBouquetManager::ChannelIterator cit = (((currentMode & RADIO_MODE) && (mode == CZapitClient::MODE_CURRENT)) || (mode==CZapitClient::MODE_RADIO)) ? g_bouquetManager->radioChannelsBegin() : g_bouquetManager->tvChannelsBegin();
		for (; !(cit.EndOfChannels()); cit++)
			channels.push_back(*cit);
	}
	else if (order == CZapitClient::SORT_ALPHA)
	{
		// ATTENTION: in this case response.nr does not return the actual number of the channel for zapping!
		if (((currentMode & RADIO_MODE) && (mode == CZapitClient::MODE_CURRENT)) || (mode == CZapitClient::MODE_RADIO)) {
			CServiceManager::getInstance()->GetAllRadioChannels(channels);
		} else {
			CServiceManager::getInstance()->GetAllTvChannels(channels);
		}
		sort(channels.begin(), channels.end(), CmpChannelByChName());
	}

	internalSendChannels(connfd, &channels, 0, false);
}

bool CZapit::StartPlayBack(CZapitChannel *thisChannel)
{
	bool have_pcr = false;
	bool have_audio = false;
	bool have_video = false;
	bool have_teletext = false;

	if(!thisChannel)
		thisChannel = current_channel;
	if ((playbackStopForced == true) || (!thisChannel) || playing)
		return false;

	printf("[zapit] vpid %X apid %X pcr %X\n", thisChannel->getVideoPid(), thisChannel->getAudioPid(), thisChannel->getPcrPid());
	if(standby) {
		CFrontend::getInstance()->Open();
		return true;
	}

	if (thisChannel->getPcrPid() != 0)
		have_pcr = true;
	if (thisChannel->getAudioPid() != 0)
		have_audio = true;
	if ((thisChannel->getVideoPid() != 0) && (currentMode & TV_MODE))
		have_video = true;
	if (thisChannel->getTeletextPid() != 0)
		have_teletext = true;

	if ((!have_audio) && (!have_video) && (!have_teletext))
		return false;
#if 1
	if(have_video && (thisChannel->getPcrPid() == 0x1FFF)) { //FIXME
		thisChannel->setPcrPid(thisChannel->getVideoPid());
		have_pcr = true;
	}
#endif
	/* set demux filters */
	videoDecoder->SetStreamType((VIDEO_FORMAT)thisChannel->type);
//	videoDecoder->SetSync(VIDEO_PLAY_MOTION);

#if HAVE_AZBOX_HARDWARE
	if (have_audio) {
		audioDemux->pesFilter(thisChannel->getAudioPid());
	}
	/* select audio output and start audio */
	if (have_audio) {
		SetAudioStreamType(thisChannel->getAudioChannel()->audioChannelType);
		audioDemux->Start();
		audioDecoder->Start();
	}
	if (have_video) {
		videoDemux->pesFilter(thisChannel->getVideoPid());
	}
	/* start video */
	if (have_video) {
		videoDemux->Start();
		videoDecoder->Start(0, thisChannel->getPcrPid(), thisChannel->getVideoPid());
	}
	if (have_pcr) {
		pcrDemux->pesFilter(thisChannel->getPcrPid());
	}
	if (have_pcr) {
		printf("[zapit] starting PCR 0x%X\n", thisChannel->getPcrPid());
		pcrDemux->Start();
	}
#else
	if (have_pcr) {
		pcrDemux->pesFilter(thisChannel->getPcrPid());
	}
	if (have_audio) {
		audioDemux->pesFilter(thisChannel->getAudioPid());
	}
	if (have_video) {
		videoDemux->pesFilter(thisChannel->getVideoPid());
	}
//	audioDecoder->SetSyncMode(AVSYNC_ENABLED);

#if 0 //FIXME hack ?
	if(thisChannel->getServiceType() == ST_DIGITAL_RADIO_SOUND_SERVICE) {
		audioDecoder->SetSyncMode(AVSYNC_AUDIO_IS_MASTER);
		have_pcr = false;
	}
#endif
	if (have_pcr) {
		printf("[zapit] starting PCR 0x%X\n", thisChannel->getPcrPid());
		pcrDemux->Start();
	}

	/* select audio output and start audio */
	if (have_audio) {
		SetAudioStreamType(thisChannel->getAudioChannel()->audioChannelType);
		audioDemux->Start();
		audioDecoder->Start();
	}

	/* start video */
	if (have_video) {
		videoDecoder->Start(0, thisChannel->getPcrPid(), thisChannel->getVideoPid());
		videoDemux->Start();
	}
#ifdef USE_VBI
	if(have_teletext)
		videoDecoder->StartVBI(thisChannel->getTeletextPid());
#endif
#endif
	playing = true;

	return true;
}

bool CZapit::StopPlayBack(bool send_pmt)
{
	if(send_pmt) {
		CCamManager::getInstance()->Stop(live_channel_id, CCamManager::PLAY);
		ca->SendPMT(0, (unsigned char*) "", 0, CA_SLOT_TYPE_SMARTCARD);
	}

	printf("StopPlayBack: standby %d forced %d\n", standby, playbackStopForced);

	if (!playing)
		return true;

	if (playbackStopForced)
		return false;

#if HAVE_AZBOX_HARDWARE
	pcrDemux->Stop();
	audioDemux->Stop();
	videoDemux->Stop();
#else
	videoDemux->Stop();
	audioDemux->Stop();
	pcrDemux->Stop();
#endif
	audioDecoder->Stop();

	/* hack. if standby, dont blank video -> for paused timeshift */
	videoDecoder->Stop(standby ? false : true);
#ifdef USE_VBI
	videoDecoder->StopVBI();
#endif
	playing = false;

	tuxtx_stop_subtitle();
	if(standby)
		dvbsub_pause();
	else
		dvbsub_stop();

	return true;
}

void CZapit::enterStandby(void)
{
	if (standby)
		return;

	standby = true;

	SaveSettings(true, true);
	StopPlayBack(true);

	if(!(currentMode & RECORD_MODE)) {
		CFrontend::getInstance()->Close();
	}
}

void CZapit::leaveStandby(void)
{
	if(!standby)
		return;

	printf("[zapit] diseqc type = %d\n", diseqcType);

	CFrontend::getInstance()->setDiseqcRepeats(configfile.getInt32("diseqcRepeats", 0));
	CFrontend::getInstance()->setCurrentSatellitePosition(configfile.getInt32("lastSatellitePosition", 0));
	CFrontend::getInstance()->setDiseqcType(diseqcType);

	if(!(currentMode & RECORD_MODE)) {
		CFrontend::getInstance()->Open();
		CFrontend::getInstance()->setTsidOnid(0);
		CFrontend::getInstance()->setDiseqcType(diseqcType);
	}
	standby = false;
	if (current_channel)
		/* tune channel, with stopped playback to not bypass the parental PIN check */
		ZapIt(live_channel_id, false, false);
}

unsigned CZapit::ZapTo(const unsigned int bouquet, const unsigned int pchannel)
{
	if (bouquet >= g_bouquetManager->Bouquets.size()) {
		WARN("Invalid bouquet %d", bouquet);
		return CZapitClient::ZAP_INVALID_PARAM;
	}

	ZapitChannelList *channels;

	if (currentMode & RADIO_MODE)
		channels = &(g_bouquetManager->Bouquets[bouquet]->radioChannels);
	else
		channels = &(g_bouquetManager->Bouquets[bouquet]->tvChannels);

	if (pchannel >= channels->size()) {
		WARN("Invalid channel %d in bouquet %d", pchannel, bouquet);
		return CZapitClient::ZAP_INVALID_PARAM;
	}

	return ZapTo((*channels)[pchannel]->getChannelID(), false);
}

unsigned int CZapit::ZapTo(t_channel_id channel_id, bool isSubService)
{
	unsigned int result = 0;

	if (!ZapIt(channel_id)) {
		DBG("[zapit] zapit failed, chid %llx\n", channel_id);
		SendEvent((isSubService ? CZapitClient::EVT_ZAP_SUB_FAILED : CZapitClient::EVT_ZAP_FAILED), &channel_id, sizeof(channel_id));
		return result;
	}

	result |= CZapitClient::ZAP_OK;

	DBG("[zapit] zapit OK, chid %llx\n", channel_id);
	if (isSubService) {
		DBG("[zapit] isSubService chid %llx\n", channel_id);
		SendEvent(CZapitClient::EVT_ZAP_SUB_COMPLETE, &channel_id, sizeof(channel_id));
	}
	else if (current_is_nvod) {
		DBG("[zapit] NVOD chid %llx\n", channel_id);
		SendEvent(CZapitClient::EVT_ZAP_COMPLETE_IS_NVOD, &channel_id, sizeof(channel_id));
		result |= CZapitClient::ZAP_IS_NVOD;
	}
	else
		SendEvent(CZapitClient::EVT_ZAP_COMPLETE, &channel_id, sizeof(channel_id));

	return result;
}

unsigned CZapit::ZapTo(const unsigned int pchannel)
{
	CBouquetManager::ChannelIterator cit = ((currentMode & RADIO_MODE) ? g_bouquetManager->radioChannelsBegin() : g_bouquetManager->tvChannelsBegin()).FindChannelNr(pchannel);
	if (!(cit.EndOfChannels()))
		return ZapTo((*cit)->getChannelID(), false);
	else
		return 0;
}

bool CZapit::Start(Z_start_arg *ZapStart_arg)
{
	if(started)
		return false;

	/* load configuration or set defaults if no configuration file exists */
	video_mode = ZapStart_arg->video_mode;

	videoDemux = new cDemux();
	videoDemux->Open(DMX_VIDEO_CHANNEL);

	pcrDemux = new cDemux();
	pcrDemux->Open(DMX_PCR_ONLY_CHANNEL, videoDemux->getBuffer());

	audioDemux = new cDemux();
	audioDemux->Open(DMX_AUDIO_CHANNEL);

	videoDecoder = new cVideo(video_mode, videoDemux->getChannel(), videoDemux->getBuffer());
	videoDecoder->Standby(false);

	audioDecoder = new cAudio(audioDemux->getBuffer(), videoDecoder->GetTVEnc(), NULL /*videoDecoder->GetTVEncSD()*/);
	videoDecoder->SetAudioHandle(audioDecoder->GetHandle());

#ifdef USE_VBI
	videoDecoder->OpenVBI(1);
#endif
	ca = cCA::GetInstance();

	LoadSettings();
	ConfigFrontend();

	/* create bouquet manager */
	g_bouquetManager = new CBouquetManager();

	if (configfile.getInt32("lastChannelMode", 0))
		SetRadioMode();
	else
		SetTVMode();

	if(ZapStart_arg->uselastchannel == 0){
		live_channel_id = (currentMode & RADIO_MODE) ? ZapStart_arg->startchannelradio_id : ZapStart_arg->startchanneltv_id ;
		lastChannelRadio = ZapStart_arg->startchannelradio_nr;
		lastChannelTV    = ZapStart_arg->startchanneltv_nr;
	}

	/* CA_INIT_CI or CA_INIT_SC or CA_INIT_BOTH */
	switch(cam_ci){
	  case 2:
		ca->SetInitMask(CA_INIT_BOTH);
	    break;
	  case 1:
		ca->SetInitMask(CA_INIT_CI);
	    break;
	  case 0:
		ca->SetInitMask(CA_INIT_SC);
	    break;
	  default:
		ca->SetInitMask(CA_INIT_BOTH);
	  break;
	}
	ca->Start();

	eventServer = new CEventServer;

	if (!PrepareChannels())
		WARN("error parsing services");
	else
		INFO("channels have been loaded succesfully");

	current_channel = CServiceManager::getInstance()->FindChannel(live_channel_id);

#ifndef EVOLUX
	// some older? hw needs this sleep. e.g. my hd-1c.
	// if sleep is not set -> blackscreen after boot.
	// sleep(1) is ok here. (striper)
	sleep(1);
#endif
	leaveStandby();

	started = true;
	int ret = start();
	return (ret == 0);
}

bool CZapit::Stop()
{
	if(!started)
		return false;
	started = false;
	int ret = join();
	return (ret == 0);
}

static bool zapit_parse_command(CBasicMessage::Header &rmsg, int connfd)
{
	return CZapit::getInstance()->ParseCommand(rmsg, connfd);
}

void CZapit::run()
{
#if 0
	time_t stime = time(0);
	time_t curtime;
#endif
#if HAVE_SPARK_HARDWARE
	bool v_stopped = false;
#endif
	printf("[zapit] starting... tid %ld\n", syscall(__NR_gettid));

	abort_zapit = 0;

	CBasicServer zapit_server;

	if (!zapit_server.prepare(ZAPIT_UDS_NAME)) {
		perror(ZAPIT_UDS_NAME);
		return;
	}

	//pthread_create (&tsdt, NULL, sdt_thread, (void *) NULL);
	SdtMonitor.Start();
	while (started && zapit_server.run(zapit_parse_command, CZapitMessages::ACTVERSION, true))
	{
		if (pmt_update_fd != -1) {
			unsigned char buf[4096];
			int ret = pmtDemux->Read(buf, 4095, 10);
				if (ret > 0) {
					pmt_stop_update_filter(&pmt_update_fd);
					printf("[zapit] pmt updated, sid 0x%x new version 0x%x\n", (buf[3] << 8) + buf[4], (buf[5] >> 1) & 0x1F);
					if(current_channel) {
						t_channel_id channel_id = current_channel->getChannelID();
						int vpid = current_channel->getVideoPid();
						parse_pmt(current_channel);
						if(vpid != current_channel->getVideoPid()) {
							ZapIt(current_channel->getChannelID(), true);
						} else {
							SendPMT(true);
							pmt_set_update_filter(current_channel, &pmt_update_fd);
						}
						SendEvent(CZapitClient::EVT_PMT_CHANGED, &channel_id, sizeof(channel_id));
					}
				}
#if HAVE_SPARK_HARDWARE
			/* hack: stop videodecoder if the tuner looses lock
			 * at least the h264 decoder seems unhappy if he runs out of data...
			 * ...until we fix the driver, let's work around it here.
			 * theoretically, a "retune()" function could also be implemented here
			 * for the case that the driver cannot re-lock the tuner (DiSEqC problem,
			 * unicable collision, .... */
			if (CFrontend::getInstance()->tuned)
			{
				if (!CFrontend::getInstance()->getStatus() && !v_stopped)
				{
					fprintf(stderr, "[zapit] LOST LOCK! stopping video...\n");
					videoDecoder->Stop(false);
					v_stopped = true;
				}
				else if (CFrontend::getInstance()->getStatus())
				{
					if (v_stopped)
					{
						fprintf(stderr, "[zapit] reacquired LOCK! starting video...\n");
						videoDecoder->Start();
					}
					v_stopped = false;
				}
			}
#endif
		}
		/* yuck, don't waste that much cpu time :) */
		usleep(0);
#if 0
		if(!standby && !CServiceScan::getInstance()->Scanning() &&current_channel) {
			curtime = time(0);
			if(sig_delay && (curtime - stime) > sig_delay) {
				stime = curtime;
				uint16_t sig  = CFrontend::getInstance()->getSignalStrength();
				//if(sig < 8000)
				if(sig < 28000) {
					printf("[monitor] signal %d, trying to re-tune...\n", sig);
					CFrontend::getInstance()->retuneChannel();
				}
			}
		}
#endif
	}

	SaveChannelPids(current_channel);
	SaveSettings(true, true);
	StopPlayBack(true);

	CServiceManager::getInstance()->SaveMotorPositions();

	SdtMonitor.Stop();
	INFO("shutdown started");
#ifdef USE_VBI
	videoDecoder->CloseVBI();
#endif
	delete pcrDemux;
	delete pmtDemux;
	delete audioDecoder;
	delete audioDemux;

	INFO("demuxes/decoders deleted");

	CFrontend::getInstance()->Close();
	delete CFrontend::getInstance();

	INFO("frontend deleted");
	if (ca) {
		INFO("stopping CA");
		ca->Stop();
		delete ca;
	}
	INFO("shutdown complete");
	return;
}

void CZapit::SetConfig(Zapit_config * Cfg)
{
	printf("[zapit] %s...\n", __FUNCTION__);

	config = *Cfg;

	//FIXME globals!
#if 0
	motorRotationSpeed = config.motorRotationSpeed;
	highVoltage = config.highVoltage;
	feTimeout = config.feTimeout;
	gotoXXLaDirection = config.gotoXXLaDirection;
	gotoXXLoDirection = config.gotoXXLoDirection;
	gotoXXLatitude = config.gotoXXLatitude;
	gotoXXLongitude = config.gotoXXLongitude;
	repeatUsals = config.repeatUsals;
#endif
	SaveSettings(true, false);
	ConfigFrontend();
}

void CZapit::SendConfig(int connfd)
{
	printf("[zapit] %s...\n", __FUNCTION__);
	CBasicServer::send_data(connfd, &config, sizeof(config));
}

void CZapit::GetConfig(Zapit_config &Cfg)
{
	printf("[zapit] %s...\n", __FUNCTION__);
	Cfg = config;
}

CZapitSdtMonitor::CZapitSdtMonitor()
{
	sdt_wakeup = false;
	started = false;
}

CZapitSdtMonitor::~CZapitSdtMonitor()
{
	Stop();
}

void CZapitSdtMonitor::Wakeup()
{
	sdt_wakeup = true;
}

bool CZapitSdtMonitor::Start()
{
	started = true;
	int ret = start();
	return (ret == 0);
}

bool CZapitSdtMonitor::Stop()
{
	started = false;
	int ret = join();
	return (ret == 0);
}

void CZapitSdtMonitor::run()
{
	time_t tstart, tcur, wtime = 0;
	int ret;
	t_transport_stream_id           transport_stream_id = 0;
	t_original_network_id           original_network_id = 0;
	t_satellite_position            satellitePosition = 0;
	freq_id_t                       freq = 0;
	transponder_id_t 		tpid = 0;

	tcur = time(0);
	tstart = time(0);
	sdt_tp.clear();
	printf("[zapit] sdt monitor started\n");
	while(started) {
		sleep(1);
		if(sdt_wakeup) {
			sdt_wakeup = 0;
			CZapitChannel * channel = CZapit::getInstance()->GetCurrentChannel();
			if(channel) {
				wtime = time(0);
				transport_stream_id = channel->getTransportStreamId();
				original_network_id = channel->getOriginalNetworkId();
				satellitePosition = channel->getSatellitePosition();
				freq = channel->getFreqId();
				tpid = channel->getTransponderId();
			}
		}
		if(!CZapit::getInstance()->scanSDT())
			continue;

		tcur = time(0);
		if(wtime && ((tcur - wtime) > 2) && !sdt_wakeup) {
			printf("[sdt monitor] wakeup...\n");
			wtime = 0;

			if(CServiceScan::getInstance()->Scanning() || CZapit::getInstance()->Recording())
				continue;

			transponder_list_t::iterator tI = transponders.find(tpid);
			if(tI == transponders.end()) {
				printf("[sdt monitor] tp not found ?!\n");
				continue;
			}
			sdt_tp_map_t::iterator stI = sdt_tp.find(tpid);

			if ((stI != sdt_tp.end()) && ((time_monotonic() - stI->second) < 3600)) {
				printf("[sdt monitor] TP already updated less than an hour ago.\n");
				continue;
			}

			CServiceManager::getInstance()->RemoveCurrentChannels();

			ret = parse_current_sdt(transport_stream_id, original_network_id, satellitePosition, freq);
			if(ret) {
				if(ret == -1)
					printf("[sdt monitor] scanSDT broken ?\n");
				continue;
			}
			sdt_tp.insert(std::pair <transponder_id_t, time_t> (tpid, time_monotonic()));

			bool updated = CServiceManager::getInstance()->SaveCurrentServices(tpid);

			if(updated && (CZapit::getInstance()->scanSDT() == 1))
				CZapit::getInstance()->SendEvent(CZapitClient::EVT_SDT_CHANGED);
			if(!updated)
				printf("[sdt monitor] no changes.\n");
			else
				printf("[sdt monitor] found changes.\n");
		}
	}
	return;
}
