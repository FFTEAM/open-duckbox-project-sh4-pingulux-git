#ifndef __configure_network_h__
#define __configure_network_h__

/*
 * $Header: /cvsroot/tuxbox/apps/tuxbox/neutrino/src/system/configure_network.h,v 1.3 2003/03/10 21:22:41 thegoodguy Exp $
 *
 * (C) 2003 by thegoodguy <thegoodguy@berlios.de>
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

#include <string>

class CNetworkConfig
{
 private:
	bool        orig_automatic_start;
	std::string orig_address;
	std::string orig_netmask;
	std::string orig_broadcast;
	std::string orig_gateway;
	std::string orig_nameserver;
	std::string orig_interface;
	bool        orig_inet_static;

	std::string orig_wlan_essid;
	std::string orig_wlan_key;
	std::string orig_wlan_mode;

	void copy_to_orig(void);
	void copy_from_orig(void);
	bool modified_from_orig(void);

 public:
	bool        automatic_start;
	std::string address;
	std::string netmask;
	std::string broadcast;
	std::string gateway;
	std::string nameserver;
	std::string interface;
	bool        inet_static;

	std::string wlan_essid;
	std::string wlan_key;
	std::string wlan_mode;

	char active_interface[40];

	CNetworkConfig(void);

	void getInterfaceConfig(bool = true);
	void getDHCPInterfaceConfig(void);

	void commitConfig(void);

	void startNetwork(void);
	void stopNetwork(void);

	void setBroadcast(void);
	int setWLAN(void);
	bool isWireless (void);
};

#endif /* __configure_network_h__ */
