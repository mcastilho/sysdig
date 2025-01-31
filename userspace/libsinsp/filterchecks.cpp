/*
Copyright (C) 2013-2014 Draios inc.

This file is part of sysdig.

sysdig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

sysdig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with sysdig.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <time.h>
#ifndef _WIN32
#include <algorithm>
#endif
#include "sinsp.h"
#include "sinsp_int.h"

#ifdef HAS_FILTERING
#include "filter.h"
#include "filterchecks.h"
#include "protodecoder.h"

extern sinsp_evttables g_infotables;

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_fd implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_fd_fields[] =
{
	{PT_INT64, EPF_NONE, PF_ID, "fd.num", "the unique number identifying the file descriptor."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "fd.type", "type of FD. Can be 'file', 'directory', 'ipv4', 'ipv6', 'unix', 'pipe', 'event', 'signalfd', 'eventpoll', 'inotify' or 'signalfd'."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "fd.typechar", "type of FD as a single character. Can be 'f' for file, 4 for IPv4 socket, 6 for IPv6 socket, 'u' for unix socket, p for pipe, 'e' for eventfd, 's' for signalfd, 'l' for eventpoll, 'i' for inotify, 'o' for uknown."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.name", "FD full name. If the fd is a file, this field contains the full path. If the FD is a socket, this field contain the connection tuple."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.directory", "If the fd is a file, the directory that contains it."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.filename", "If the fd is a file, the filename without the path."},
	{PT_IPV4ADDR, EPF_NONE, PF_NA, "fd.ip", "matches the ip address (client or server) of the fd."},
	{PT_IPV4ADDR, EPF_NONE, PF_NA, "fd.cip", "client IP address."},
	{PT_IPV4ADDR, EPF_NONE, PF_NA, "fd.sip", "server IP address."},
	{PT_IPV4ADDR, EPF_NONE, PF_NA, "fd.lip", "local IP address."},
	{PT_IPV4ADDR, EPF_NONE, PF_NA, "fd.rip", "remote IP address."},
	{PT_PORT, EPF_FILTER_ONLY, PF_DEC, "fd.port", "matches the port (either client or server) of the fd."},
	{PT_PORT, EPF_NONE, PF_DEC, "fd.cport", "for TCP/UDP FDs, the client port."},
	{PT_PORT, EPF_NONE, PF_DEC, "fd.sport", "for TCP/UDP FDs, server port."},
	{PT_PORT, EPF_NONE, PF_DEC, "fd.lport", "for TCP/UDP FDs, the local port."},
	{PT_PORT, EPF_NONE, PF_DEC, "fd.rport", "for TCP/UDP FDs, the remote port."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.l4proto", "the IP protocol of a socket. Can be 'tcp', 'udp', 'icmp' or 'raw'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.sockfamily", "the socket family for socket events. Can be 'ip' or 'unix'."},
	{PT_BOOL, EPF_NONE, PF_NA, "fd.is_server", "'true' if the process owning this FD is the server endpoint in the connection."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.uid", "a unique identifier for the FD, created by chaining the FD number and the thread ID."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.containername", "chaining of the container ID and the FD name. Useful when trying to identify which container an FD belongs to."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.containerdirectory", "chaining of the container ID and the directory name. Useful when trying to identify which container a directory belongs to."},
};

sinsp_filter_check_fd::sinsp_filter_check_fd()
{
	m_tinfo = NULL;
	m_fdinfo = NULL;

	m_info.m_name = "fd";
	m_info.m_fields = sinsp_filter_check_fd_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_fd_fields) / sizeof(sinsp_filter_check_fd_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_fd::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_fd();
}

int32_t sinsp_filter_check_fd::parse_field_name(const char* str, bool alloc_state)
{
	return sinsp_filter_check::parse_field_name(str, alloc_state);
}

bool sinsp_filter_check_fd::extract_fdname_from_creator(sinsp_evt *evt, OUT uint32_t* len)
{
	const char* resolved_argstr;
	uint16_t etype = evt->get_type();

	if(PPME_IS_ENTER(etype))
	{
		return false;
	}

	switch(etype)
	{
	case PPME_SYSCALL_OPEN_X:
	case PPME_SOCKET_ACCEPT_X:
	case PPME_SOCKET_ACCEPT_5_X:
	case PPME_SOCKET_ACCEPT4_X:
	case PPME_SOCKET_ACCEPT4_5_X:
	case PPME_SYSCALL_CREAT_X:
		{
			const char* argstr = evt->get_param_as_str(1, &resolved_argstr, 
				m_inspector->get_buffer_format());
			
			if(resolved_argstr[0] != 0)
			{
				m_tstr = resolved_argstr;
			}
			else
			{
				m_tstr = argstr;
			}

			return true;
		}
	case PPME_SOCKET_CONNECT_X:
		{
			const char* argstr = evt->get_param_as_str(1, &resolved_argstr, 
				m_inspector->get_buffer_format());
			
			if(resolved_argstr[0] != 0)
			{
				m_tstr = resolved_argstr;
			}
			else
			{
				m_tstr = argstr;
			}

			return true;
		}
	case PPME_SYSCALL_OPENAT_X:
		{
			//
			// XXX This is highly inefficient, as it re-requests the enter event and then
			// does unnecessary allocations and copies. We assume that failed openat() happen
			// rarely enough that we don't care.
			//
			sinsp_evt enter_evt;
			if(!m_inspector->get_parser()->retrieve_enter_event(&enter_evt, evt))
			{
				return false;
			}

			sinsp_evt_param *parinfo;
			char *name;
			uint32_t namelen;
			string sdir;

			parinfo = enter_evt.get_param(1);
			name = parinfo->m_val;
			namelen = parinfo->m_len;

			parinfo = enter_evt.get_param(0);
			ASSERT(parinfo->m_len == sizeof(int64_t));
			int64_t dirfd = *(int64_t *)parinfo->m_val;

			sinsp_parser::parse_openat_dir(evt, name, dirfd, &sdir);

			char fullpath[SCAP_MAX_PATH_SIZE];

			sinsp_utils::concatenate_paths(fullpath, SCAP_MAX_PATH_SIZE, 
				sdir.c_str(), 
				(uint32_t)sdir.length(), 
				name, 
				namelen);
	
			m_tstr = fullpath;
			m_tstr.erase(remove_if(m_tstr.begin(), m_tstr.end(), g_invalidchar()), m_tstr.end());
			return true;
		}
	default:
		m_tstr = "";
		return true;
	}
}

uint8_t* sinsp_filter_check_fd::extract_from_null_fd(sinsp_evt *evt, OUT uint32_t* len)
{
	//
	// Even is there's no fd, we still try to extract a name from exit events that create
	// one. With these events, the fact that there's no FD means that the call failed,
	// but even if that happened we still want to collect the name.
	//
	switch(m_field_id)
	{
	case TYPE_FDNAME:
	{
		if(extract_fdname_from_creator(evt, len) == true)
		{
			return (uint8_t*)m_tstr.c_str();
		}
		else
		{
			return NULL;
		}
	}
	case TYPE_CONTAINERNAME:
	{
		if(extract_fdname_from_creator(evt, len) == true)
		{
			m_tstr = m_tinfo->m_container_id + ':' + m_tstr;
			return (uint8_t*)m_tstr.c_str();
		}
		else
		{
			return NULL;
		}
	}
	case TYPE_DIRECTORY:
	{
		if(extract_fdname_from_creator(evt, len) == true)
		{
			m_tstr.erase(remove_if(m_tstr.begin(), m_tstr.end(), g_invalidchar()), m_tstr.end());

			size_t pos = m_tstr.rfind('/');
			if(pos != string::npos)
			{
				if(pos < m_tstr.size() - 1)
				{
					m_tstr.resize(pos);
				}
			}
			else
			{
				m_tstr = "/";
			}

			return (uint8_t*)m_tstr.c_str();
		}
		else
		{
			return NULL;
		}
	}
	case TYPE_CONTAINERDIRECTORY:
	{
		if(extract_fdname_from_creator(evt, len) == true)
		{
			m_tstr.erase(remove_if(m_tstr.begin(), m_tstr.end(), g_invalidchar()), m_tstr.end());

			size_t pos = m_tstr.rfind('/');
			if(pos != string::npos)
			{
				if(pos < m_tstr.size() - 1)
				{
					m_tstr.resize(pos);
				}
			}
			else
			{
				m_tstr = "/";
			}

			m_tstr = m_tinfo->m_container_id + ':' + m_tstr;
			return (uint8_t*)m_tstr.c_str();
		}
		else
		{
			return NULL;
		}
	}
	case TYPE_FILENAME:
	{
		if(evt->get_type() != PPME_SYSCALL_OPEN_E && evt->get_type() != PPME_SYSCALL_OPENAT_E &&
			evt->get_type() != PPME_SYSCALL_CREAT_E)
		{
			return NULL;
		}
 
		if(extract_fdname_from_creator(evt, len) == true)
		{
			m_tstr.erase(remove_if(m_tstr.begin(), m_tstr.end(), g_invalidchar()), m_tstr.end());

			size_t pos = m_tstr.rfind('/');
			if(pos != string::npos)
			{
				if(pos < m_tstr.size() - 1)
				{
					m_tstr = m_tstr.substr(pos + 1, string::npos);
				}
			}

			return (uint8_t*)m_tstr.c_str();
		}
		else
		{
			return NULL;
		}
	}
	case TYPE_FDTYPECHAR:
		switch(PPME_MAKE_ENTER(evt->get_type()))
		{
		case PPME_SYSCALL_OPEN_E:
		case PPME_SYSCALL_OPENAT_E:
		case PPME_SYSCALL_CREAT_E:
			m_tcstr[0] = CHAR_FD_FILE;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SOCKET_SOCKET_E:
		case PPME_SOCKET_ACCEPT_E:
		case PPME_SOCKET_ACCEPT_5_E:
		case PPME_SOCKET_ACCEPT4_E:
		case PPME_SOCKET_ACCEPT4_5_E:
                	//
                	// Note, this is not accurate, because it always
                	// returns IPv4 even if this could be IPv6 or unix.
                	// For the moment, I assume it's better than nothing, and doing
                	// real event parsing here would be a pain.
                	//
                	m_tcstr[0] = CHAR_FD_IPV4_SOCK;
                	m_tcstr[1] = 0;
                	return m_tcstr;
		case PPME_SYSCALL_PIPE_E:
			m_tcstr[0] = CHAR_FD_FIFO;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SYSCALL_EVENTFD_E:
			m_tcstr[0] = CHAR_FD_EVENT;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SYSCALL_SIGNALFD_E:
			m_tcstr[0] = CHAR_FD_SIGNAL;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SYSCALL_TIMERFD_CREATE_E:
			m_tcstr[0] = CHAR_FD_TIMERFD;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SYSCALL_INOTIFY_INIT_E:
			m_tcstr[0] = CHAR_FD_INOTIFY;
			m_tcstr[1] = 0;
			return m_tcstr;
		default:
			m_tcstr[0] = 'o';
			m_tcstr[1] = 0;
			return m_tcstr;
		}
	default:
		return NULL;
	}
}

uint8_t* sinsp_filter_check_fd::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	ASSERT(evt);

	if(!extract_fd(evt))
	{
		return NULL;
	}

	//
	// TYPE_FDNUM doesn't need fdinfo
	//
	if(m_field_id == TYPE_FDNUM)
	{
		if(m_fdinfo != NULL)
		{
			return (uint8_t*)&m_tinfo->m_lastevent_fd;
		}
		else
		{
			return NULL;
		}
	}

	switch(m_field_id)
	{
	case TYPE_FDNAME:
	case TYPE_CONTAINERNAME:
		if(m_fdinfo == NULL)
		{
			return extract_from_null_fd(evt, len);
		}

		if(evt->get_type() == PPME_SOCKET_CONNECT_X)
		{
			sinsp_evt_param *parinfo;

			parinfo = evt->get_param(0);
			ASSERT(parinfo->m_len == sizeof(uint64_t));
			int64_t retval = *(int64_t*)parinfo->m_val;

			if(retval < 0)
			{
				return extract_from_null_fd(evt, len);
			}
		}

		if(m_field_id == TYPE_CONTAINERNAME)
		{
			ASSERT(m_tinfo != NULL);
			m_tstr = m_tinfo->m_container_id + ':' + m_fdinfo->m_name;
		}
		else
		{
			m_tstr = m_fdinfo->m_name;
		}

		m_tstr.erase(remove_if(m_tstr.begin(), m_tstr.end(), g_invalidchar()), m_tstr.end());
		return (uint8_t*)m_tstr.c_str();
	case TYPE_FDTYPE:
		if(m_fdinfo == NULL)
		{
			return NULL;
		}

		return (uint8_t*)m_fdinfo->get_typestring();
	case TYPE_DIRECTORY:
	case TYPE_CONTAINERDIRECTORY:
		{
			if(m_fdinfo == NULL)
			{
				return extract_from_null_fd(evt, len);
			}

			if(!(m_fdinfo->is_file() || m_fdinfo->is_directory()))
			{
				return NULL;
			}

			m_tstr = m_fdinfo->m_name;
			m_tstr.erase(remove_if(m_tstr.begin(), m_tstr.end(), g_invalidchar()), m_tstr.end());

			if(m_fdinfo->is_file())
			{
				size_t pos = m_tstr.rfind('/');
				if(pos != string::npos)
				{
					if(pos < m_tstr.size() - 1)
					{
						m_tstr.resize(pos);
					}
				}
				else
				{
					m_tstr = "/";
				}
			}

			if(m_field_id == TYPE_CONTAINERDIRECTORY)
			{
				m_tstr = m_tinfo->m_container_id + ':' + m_tstr;
			}

			return (uint8_t*)m_tstr.c_str();
		}
	case TYPE_FILENAME:
		{
			if(m_fdinfo == NULL)
			{
				return extract_from_null_fd(evt, len);
			}

			if(!m_fdinfo->is_file())
			{
				return NULL;
			}

			m_tstr = m_fdinfo->m_name;
			m_tstr.erase(remove_if(m_tstr.begin(), m_tstr.end(), g_invalidchar()), m_tstr.end());

			size_t pos = m_tstr.rfind('/');
			if(pos != string::npos)
			{
				if(pos < m_tstr.size() - 1)
				{
					m_tstr = m_tstr.substr(pos + 1, string::npos);
				}
			}
			else
			{
				m_tstr = "/";
			}

			return (uint8_t*)m_tstr.c_str();
		}
	case TYPE_FDTYPECHAR:
		if(m_fdinfo == NULL)
		{
			return extract_from_null_fd(evt, len);
		}

		m_tcstr[0] = m_fdinfo->get_typechar();
		m_tcstr[1] = 0;
		return m_tcstr;
	case TYPE_CLIENTIP:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip);
			}
		}

		break;
	case TYPE_SERVERIP:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip);
			}
			else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
			{
				return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_ip);
			}
		}

		break;
	case TYPE_LIP:
	case TYPE_RIP:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type != SCAP_FD_IPV4_SOCK)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			if(m_inspector->get_ifaddr_list()->is_ipv4addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip))
			{
				if(m_field_id == TYPE_LIP)
				{
					return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip);
				}
				else
				{
					return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip);
				}
			}
			else
			{
				if(m_field_id == TYPE_LIP)
				{
					return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip);
				}
				else
				{
					return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip);
				}
			}
		}

		break;
	case TYPE_CLIENTPORT:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport);
			}
			else if(evt_type == SCAP_FD_IPV6_SOCK)
			{
				return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport);
			}
		}
	case TYPE_SERVERPORT:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;

			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				if(m_fdinfo->is_role_none())
				{
					return NULL;
				}

				return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport);
			}
			else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
			{
				return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_port);
			}
			else if(evt_type == SCAP_FD_IPV6_SOCK)
			{
				if(m_fdinfo->is_role_none())
				{
					return NULL;
				}

				return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport);
			}
			else if(evt_type == SCAP_FD_IPV6_SERVSOCK)
			{
				return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_port);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_LPORT:
	case TYPE_RPORT:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type != SCAP_FD_IPV4_SOCK)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			if(m_inspector->get_ifaddr_list()->is_ipv4addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip))
			{
				if(m_field_id == TYPE_LPORT)
				{
					return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport);
				}
				else
				{
					return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport);
				}
			}
			else
			{
				if(m_field_id == TYPE_LPORT)
				{
					return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport);
				}
				else
				{
					return (uint8_t*)&(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport);
				}
			}
		}

		break;
	case TYPE_L4PROTO:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_l4_proto l4p = m_fdinfo->get_l4proto();

			switch(l4p)
			{
			case SCAP_L4_TCP:
				m_tstr = "tcp";
				break;
			case SCAP_L4_UDP:
				m_tstr = "udp";
				break;
			case SCAP_L4_ICMP:
				m_tstr = "icmp";
				break;
			case SCAP_L4_RAW:
				m_tstr = "raw";
				break;
			default:
				m_tstr = "<NA>";
				break;
			}

			return (uint8_t*)m_tstr.c_str();
		}
	case TYPE_IS_SERVER:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			if(m_fdinfo->m_type == SCAP_FD_IPV4_SERVSOCK || m_fdinfo->m_type == SCAP_FD_IPV6_SERVSOCK)
			{
				m_tbool = true;
			}
			else if(m_fdinfo->m_type == SCAP_FD_IPV4_SOCK)
			{
				m_tbool = 
					m_inspector->get_ifaddr_list()->is_ipv4addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip);
			}
			else
			{
				m_tbool = false;
			}

			return (uint8_t*)&m_tbool;
		}
		break;
	case TYPE_SOCKFAMILY:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			if(m_fdinfo->m_type == SCAP_FD_IPV4_SOCK || m_fdinfo->m_type == SCAP_FD_IPV6_SOCK)
			{
				m_tstr = "ip";
				return (uint8_t*)m_tstr.c_str();
			}
			else if(m_fdinfo->m_type == SCAP_FD_IPV4_SOCK || m_fdinfo->m_type == SCAP_FD_IPV6_SOCK)
			{
				m_tstr = "unix";
				return (uint8_t*)m_tstr.c_str();
			}
			else
			{
				return NULL;
			}
		}
		break;
	case TYPE_UID:
		{
			ASSERT(m_tinfo != NULL);

			m_tstr = to_string(m_tinfo->m_tid) + to_string(m_tinfo->m_lastevent_fd);
			return (uint8_t*)m_tstr.c_str();
		}
		break;
	default:
		ASSERT(false);
	}

	return NULL;
}

bool sinsp_filter_check_fd::compare_ip(sinsp_evt *evt)
{
	if(!extract_fd(evt))
	{
		return false;
	}

	if(m_fdinfo != NULL)
	{
		scap_fd_type evt_type = m_fdinfo->m_type;

		if(evt_type == SCAP_FD_IPV4_SOCK)
		{
			if(m_cmpop == CO_EQ)
			{
				if(flt_compare(m_cmpop, PT_IPV4ADDR, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, &m_val_storage[0]) ||
					flt_compare(m_cmpop, PT_IPV4ADDR, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip, &m_val_storage[0]))
				{
					return true;
				}
			}
			else if(m_cmpop == CO_NE)
			{
				if(flt_compare(m_cmpop, PT_IPV4ADDR, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, &m_val_storage[0]) &&
					flt_compare(m_cmpop, PT_IPV4ADDR, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip, &m_val_storage[0]))
				{
					return true;
				}
			}
			else
			{
				throw sinsp_exception("filter error: IP filter only supports '=' and '!=' operators");
			}
		}
		else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
		{
			if(m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_ip == *(uint32_t*)&m_val_storage[0])
			{
				return true;
			}
		}
	}

	return false;
}

bool sinsp_filter_check_fd::compare_port(sinsp_evt *evt)
{
	if(!extract_fd(evt))
	{
		return false;
	}

	if(m_fdinfo != NULL)
	{
		uint16_t* sport;
		uint16_t* dport;
		scap_fd_type evt_type = m_fdinfo->m_type;

		if(evt_type == SCAP_FD_IPV4_SOCK)
		{
			sport = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport;
			dport = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport;
		}
		else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
		{
			sport = &m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_port;
			dport = &m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_port;
		}
		else if(evt_type == SCAP_FD_IPV6_SOCK)
		{
			sport = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport;
			dport = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport;
		}
		else if(evt_type == SCAP_FD_IPV6_SERVSOCK)
		{
			sport = &m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_port;
			dport = &m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_port;
		}
		else
		{
			return false;
		}

		switch(m_cmpop)
		{
		case CO_EQ:
			if(*sport == *(uint16_t*)&m_val_storage[0] ||
				*dport == *(uint16_t*)&m_val_storage[0])
			{
				return true;
			}
			break;
		case CO_NE:
			if(*sport != *(uint16_t*)&m_val_storage[0] &&
				*dport != *(uint16_t*)&m_val_storage[0])
			{
				return true;
			}
			break;
		case CO_LT:
			if(*sport < *(uint16_t*)&m_val_storage[0] ||
				*dport < *(uint16_t*)&m_val_storage[0])
			{
				return true;
			}
			break;
		case CO_LE:
			if(*sport <= *(uint16_t*)&m_val_storage[0] ||
				*dport <= *(uint16_t*)&m_val_storage[0])
			{
				return true;
			}
			break;
		case CO_GT:
			if(*sport > *(uint16_t*)&m_val_storage[0] ||
				*dport > *(uint16_t*)&m_val_storage[0])
			{
				return true;
			}
			break;
		case CO_GE:
			if(*sport >= *(uint16_t*)&m_val_storage[0] ||
				*dport >= *(uint16_t*)&m_val_storage[0])
			{
				return true;
			}
			break;
		default:
			throw sinsp_exception("filter error: unsupported port comparison operator");
		}
	}

	return false;
}

bool sinsp_filter_check_fd::extract_fd(sinsp_evt *evt)
{
	ppm_event_flags eflags = evt->get_flags();

	//
	// Make sure this is an event that creates or consumes an fd
	//
	if(eflags & (EF_CREATES_FD | EF_USES_FD | EF_DESTROYS_FD))
	{
		//
		// This is an fd-related event, get the thread info and the fd info
		//
		m_tinfo = evt->get_thread_info();
		if(m_tinfo == NULL)
		{
			return false;
		}

		m_fdinfo = evt->get_fd_info();

		if(m_fdinfo == NULL && m_tinfo->m_lastevent_fd != -1)
		{
			m_fdinfo = m_tinfo->get_fd(m_tinfo->m_lastevent_fd);
		}

		// We'll check if fd is null below
	}
	else
	{
		return false;
	}

	return true;
}

bool sinsp_filter_check_fd::compare(sinsp_evt *evt)
{
	//
	// A couple of fields are filter only and therefore get a special treatment
	//
	if(m_field_id == TYPE_IP)
	{
		return compare_ip(evt);
	}
	else if(m_field_id == TYPE_PORT)
	{
		return compare_port(evt);
	}

	//
	// Standard extract-based fields
	//
	uint32_t len;
	uint8_t* extracted_val = extract(evt, &len);

	if(extracted_val == NULL)
	{
		return false;
	}

	return flt_compare(m_cmpop, 
		m_info.m_fields[m_field_id].m_type, 
		extracted_val, 
		&m_val_storage[0]);
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_thread implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_thread_fields[] =
{
	{PT_INT64, EPF_NONE, PF_ID, "proc.pid", "the id of the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.exe", "the first command line argument (usually the executable name or a custom one)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.name", "the name (excluding the path) of the executable generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.args", "the arguments passed on the command line when starting the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.env", "the environment variables of the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.cmdline", "full process command line, i.e. proc.name + proc.args."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.exeline", "full process command line, with exe as first argument, i.e. proc.exe + proc.args."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.cwd", "the current working directory of the event."},
	{PT_UINT32, EPF_NONE, PF_DEC, "proc.nthreads", "the number of threads that the process generating the event currently has, including the main process thread."},
	{PT_UINT32, EPF_NONE, PF_DEC, "proc.nchilds", "the number of child threads that the process generating the event currently has. This excludes the main process thread."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.ppid", "the pid of the parent of the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.pname", "the name (excluding the path) of the parent of the process generating the event."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.apid", "the pid of one of the process ancestors. E.g. proc.apid[1] returns the parent pid, proc.apid[2] returns the grandparent pid, and so on. proc.apid[0] is the pid of the current process. proc.apid without arguments can be used in filters only and matches any of the process ancestors, e.g. proc.apid=1234."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.aname", "the name (excluding the path) of one of the process ancestors. E.g. proc.aname[1] returns the parent name, proc.aname[2] returns the grandparent name, and so on. proc.aname[0] is the name of the current process. proc.aname without arguments can be used in filters only and matches any of the process ancestors, e.g. proc.aname=bash."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.loginshellid", "the pid of the oldest shell among the ancestors of the current process, if there is one. This field can be used to separate different user sessions, and is useful in conjunction with chisels like spy_user."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "proc.duration", "number of nanoseconds since the process started."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.fdopencount", "number of open FDs for the process"},
	{PT_INT64, EPF_NONE, PF_DEC, "proc.fdlimit", "maximum number of FDs the process can open."},
	{PT_DOUBLE, EPF_NONE, PF_DEC, "proc.fdusage", "the ratio between open FDs and maximum available FDs for the process."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.vmsize", "total virtual memory for the process (as kb)."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.vmrss", "resident non-swapped memory for the process (as kb)."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.vmswap", "swapped memory for the process (as kb)."},
	{PT_UINT64, EPF_NONE, PF_DEC, "thread.pfmajor", "number of major page faults since thread start."},
	{PT_UINT64, EPF_NONE, PF_DEC, "thread.pfminor", "number of minor page faults since thread start."},
	{PT_INT64, EPF_NONE, PF_ID, "thread.tid", "the id of the thread generating the event."},
	{PT_BOOL, EPF_NONE, PF_NA, "thread.ismain", "'true' if the thread generating the event is the main one in the process."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "thread.exectime", "CPU time spent by the last scheduled thread, in nanoseconds. Exported by switch events only."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "thread.totexectime", "Total CPU time, in nanoseconds since the beginning of the capture, for the current thread. Exported by switch events only."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "thread.cgroups", "all the cgroups the thread belongs to, aggregated into a single string."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "thread.cgroup", "the cgroup the thread belongs to, for a specific subsystem. E.g. thread.cgroup.cpuacct."},
	{PT_INT64, EPF_NONE, PF_ID, "thread.vtid", "the id of the thread generating the event as seen from its current PID namespace."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.vpid", "the id of the process generating the event as seen from its current PID namespace."},
	{PT_DOUBLE, EPF_NONE, PF_NA, "thread.cpu", "the CPU consumed by the thread in the last second."},
	{PT_DOUBLE, EPF_NONE, PF_NA, "thread.cpu.user", "the user CPU consumed by the thread in the last second."},
	{PT_DOUBLE, EPF_NONE, PF_NA, "thread.cpu.system", "the system CPU consumed by the thread in the last second."},
	{PT_UINT64, EPF_NONE, PF_DEC, "thread.vmsize", "For the process main thread, this is the total virtual memory for the process (as kb). For the other threads, this field is zero."},
	{PT_UINT64, EPF_NONE, PF_DEC, "thread.vmrss", "For the process main thread, this is the resident non-swapped memory for the process (as kb). For the other threads, this field is zero."},
};

sinsp_filter_check_thread::sinsp_filter_check_thread()
{
	m_info.m_name = "process";
	m_info.m_fields = sinsp_filter_check_thread_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_thread_fields) / sizeof(sinsp_filter_check_thread_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;

	m_u64val = 0;
	m_cursec_ts = 0;
}

sinsp_filter_check* sinsp_filter_check_thread::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_thread();
}

int32_t sinsp_filter_check_thread::extract_arg(string fldname, string val, OUT const struct ppm_param_info** parinfo)
{
	uint32_t parsed_len = 0;

	//
	// 'arg' and 'resarg' are handled in a custom way
	//
	if(m_field_id == TYPE_APID || m_field_id == TYPE_ANAME)
	{
		if(val[fldname.size()] == '[')
		{
			parsed_len = (uint32_t)val.find(']');
			string numstr = val.substr(fldname.size() + 1, parsed_len - fldname.size() - 1);
			m_argid = sinsp_numparser::parsed32(numstr);
			parsed_len++;
		}
		else
		{
			throw sinsp_exception("filter syntax error: " + val);
		}
	}
	else if(m_field_id == TYPE_CGROUP)
	{
		if(val[fldname.size()] == '.')
		{
			size_t endpos;
			for(endpos = fldname.size() + 1; endpos < val.length(); ++endpos)
			{
				if(!isalpha(val[endpos]) 
					&& val[endpos] != '_')
				{
					break;
				}
			}

			parsed_len = (uint32_t)endpos;
			m_argname = val.substr(fldname.size() + 1, endpos - fldname.size() - 1);
		}
		else
		{
			throw sinsp_exception("filter syntax error: " + val);
		}		
	}

	return parsed_len; 
}

int32_t sinsp_filter_check_thread::parse_field_name(const char* str, bool alloc_state)
{
	string val(str);

	if(string(val, 0, sizeof("arg") - 1) == "arg")
	{
		//
		// 'arg' is handled in a custom way
		//
		throw sinsp_exception("filter error: proc.arg filter not implemented yet");
	}
	else if(string(val, 0, sizeof("proc.apid") - 1) == "proc.apid")
	{
		m_field_id = TYPE_APID;
		m_field = &m_info.m_fields[m_field_id];

		int32_t res = 0;

		try
		{
			res = extract_arg("proc.apid", val, NULL);
		}
		catch(...)
		{
			if(val == "proc.apid")
			{
				m_argid = -1;
				res = (int32_t)val.size();
			}
		}

		return res;
	}
	else if(string(val, 0, sizeof("proc.aname") - 1) == "proc.aname")
	{
		m_field_id = TYPE_ANAME;
		m_field = &m_info.m_fields[m_field_id];

		int32_t res = 0;

		try
		{
			res = extract_arg("proc.aname", val, NULL);
		}
		catch(...)
		{
			if(val == "proc.aname")
			{
				m_argid = -1;
				res = (int32_t)val.size();
			}
		}

		return res;
	}
	else if(string(val, 0, sizeof("thread.totexectime") - 1) == "thread.totexectime")
	{
		//
		// Allocate thread storage for the value
		//
		if(alloc_state)
		{
			m_th_state_id = m_inspector->reserve_thread_memory(sizeof(uint64_t));
		}

		return sinsp_filter_check::parse_field_name(str, alloc_state);
	}
	else if(string(val, 0, sizeof("thread.cgroup") - 1) == "thread.cgroup" &&
			string(val, 0, sizeof("thread.cgroups") - 1) != "thread.cgroups")
	{
		m_field_id = TYPE_CGROUP;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("thread.cgroup", val, NULL);
	}
	else if(string(val, 0, sizeof("thread.cpu") - 1) == "thread.cpu")
	{
		if(alloc_state)
		{
			m_th_state_id = m_inspector->reserve_thread_memory(sizeof(uint64_t));
		}

		return sinsp_filter_check::parse_field_name(str, alloc_state);
	}
	else
	{
		return sinsp_filter_check::parse_field_name(str, alloc_state);
	}
}

uint64_t sinsp_filter_check_thread::extract_exectime(sinsp_evt *evt) 
{
	uint64_t res = 0;

	if(m_last_proc_switch_times.size() == 0)
	{
		//
		// Initialize the vector of CPU times
		//
		const scap_machine_info* minfo = m_inspector->get_machine_info();
		ASSERT(minfo->num_cpus != 0);

		for(uint32_t j = 0; j < minfo->num_cpus; j++)
		{
			m_last_proc_switch_times.push_back(0);
		}
	}

	uint32_t cpuid = evt->get_cpuid();
	uint64_t ts = evt->get_ts();
	uint64_t lasttime = m_last_proc_switch_times[cpuid];

	if(lasttime != 0)
	{
		res = ts - lasttime;
	}

	ASSERT(cpuid < m_last_proc_switch_times.size());

	m_last_proc_switch_times[cpuid] = ts;

	return res;
}

uint8_t* sinsp_filter_check_thread::extract_thread_cpu(sinsp_evt *evt, sinsp_threadinfo* tinfo, bool extract_user, bool extract_system)
{
	uint16_t etype = evt->get_type();

	if(etype == PPME_PROCINFO_E)
	{
		uint64_t user = 0;
		uint64_t system = 0;
		uint64_t tcpu;

		if(extract_user)
		{
			sinsp_evt_param* parinfo = evt->get_param(0);
			user = *(uint64_t*)parinfo->m_val;
		}

		if(extract_system)
		{
			sinsp_evt_param* parinfo = evt->get_param(1);
			system = *(uint64_t*)parinfo->m_val;
		}

		tcpu = user + system;

		uint64_t* last_t_tot_cpu = (uint64_t*)tinfo->get_private_state(m_th_state_id);
		if(*last_t_tot_cpu != 0)
		{
			uint64_t deltaval = tcpu - *last_t_tot_cpu;
			m_dval = (double)deltaval;// / (ONE_SECOND_IN_NS / 100);
			if(m_dval > 100)
			{
				m_dval = 100;
			}
		}
		else
		{
			m_dval = 0;
		}

		*last_t_tot_cpu = tcpu;

		return (uint8_t*)&m_dval;
	}

	return NULL;
}

uint8_t* sinsp_filter_check_thread::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL && 
		m_field_id != TYPE_TID &&
		m_field_id != TYPE_EXECTIME &&
		m_field_id != TYPE_TOTEXECTIME)
	{
		return NULL;
	}

	switch(m_field_id)
	{
	case TYPE_TID:
		m_u64val = evt->get_tid();
		return (uint8_t*)&m_u64val;
	case TYPE_PID:
		return (uint8_t*)&tinfo->m_pid;
	case TYPE_NAME:
		m_tstr = tinfo->get_comm();
		return (uint8_t*)m_tstr.c_str();
	case TYPE_EXE:
		m_tstr = tinfo->get_exe();
		return (uint8_t*)m_tstr.c_str();
	case TYPE_ARGS:
		{
			m_tstr.clear();

			uint32_t j;
			uint32_t nargs = (uint32_t)tinfo->m_args.size();

			for(j = 0; j < nargs; j++)
			{
				m_tstr += tinfo->m_args[j];
				if(j < nargs -1)
				{
					m_tstr += ' ';
				}
			}

			return (uint8_t*)m_tstr.c_str();
		}
	case TYPE_ENV:
		{
			m_tstr.clear();

			uint32_t j;
			uint32_t nargs = (uint32_t)tinfo->m_env.size();

			for(j = 0; j < nargs; j++)
			{
				m_tstr += tinfo->m_env[j];
				if(j < nargs -1)
				{
					m_tstr += ' ';
				}
			}

			return (uint8_t*)m_tstr.c_str();
		}
	case TYPE_CMDLINE:
		{
			m_tstr = tinfo->get_comm() + " ";

			uint32_t j;
			uint32_t nargs = (uint32_t)tinfo->m_args.size();

			for(j = 0; j < nargs; j++)
			{
				m_tstr += tinfo->m_args[j];
				if(j < nargs -1)
				{
					m_tstr += ' ';
				}
			}

			return (uint8_t*)m_tstr.c_str();
		}
	case TYPE_EXELINE:
		{
			m_tstr = tinfo->get_exe() + " ";

			uint32_t j;
			uint32_t nargs = (uint32_t)tinfo->m_args.size();

			for(j = 0; j < nargs; j++)
			{
				m_tstr += tinfo->m_args[j];
				if(j < nargs -1)
				{
					m_tstr += ' ';
				}
			}

			return (uint8_t*)m_tstr.c_str();
		}
	case TYPE_CWD:
		m_tstr = tinfo->get_cwd();
		return (uint8_t*)m_tstr.c_str();
	case TYPE_NTHREADS:
		{
			sinsp_threadinfo* ptinfo = tinfo->get_main_thread();
			if(ptinfo)
			{
				m_u64val = ptinfo->m_nchilds + 1;
				return (uint8_t*)&m_u64val;
			}
			else
			{
				ASSERT(false);
				return NULL;
			}
		}
	case TYPE_NCHILDS:
		return (uint8_t*)&tinfo->m_nchilds;
	case TYPE_ISMAINTHREAD:
		m_tbool = (uint32_t)tinfo->is_main_thread();
		return (uint8_t*)&m_tbool;
	case TYPE_EXECTIME:
		{
			m_u64val = 0;
			uint16_t etype = evt->get_type();

			if(etype == PPME_SCHEDSWITCH_1_E || etype == PPME_SCHEDSWITCH_6_E)
			{
				m_u64val = extract_exectime(evt);
			}

			return (uint8_t*)&m_u64val;
		}
	case TYPE_TOTEXECTIME:
		{
			m_u64val = 0;
			uint16_t etype = evt->get_type();

			if(etype == PPME_SCHEDSWITCH_1_E || etype == PPME_SCHEDSWITCH_6_E)
			{
				m_u64val = extract_exectime(evt);
			}

			sinsp_threadinfo* tinfo = evt->get_thread_info(false);

			if(tinfo != NULL)
			{
				uint64_t* ptot = (uint64_t*)tinfo->get_private_state(m_th_state_id);
				*ptot += m_u64val;
				return (uint8_t*)ptot;
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_PPID:
		if(tinfo->is_main_thread())
		{
			return (uint8_t*)&tinfo->m_ptid;
		}
		else
		{
			sinsp_threadinfo* mt = tinfo->get_main_thread();

			if(mt != NULL)
			{
				return (uint8_t*)&mt->m_ptid;
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_PNAME:
		{
			sinsp_threadinfo* ptinfo = 
				m_inspector->get_thread(tinfo->m_ptid, false, true);

			if(ptinfo != NULL)
			{
				m_tstr = ptinfo->get_comm();
				return (uint8_t*)m_tstr.c_str();
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_APID:
		{
			sinsp_threadinfo* mt = NULL;

			if(tinfo->is_main_thread())
			{
				mt = tinfo;
			}
			else
			{
				mt = tinfo->get_main_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			//
			// Search for a specific ancestors
			//
			for(int32_t j = 0; j < m_argid; j++)
			{
				mt = mt->get_parent_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			return (uint8_t*)&mt->m_pid;
		}
	case TYPE_ANAME:
		{
			sinsp_threadinfo* mt = NULL;

			if(tinfo->is_main_thread())
			{
				mt = tinfo;
			}
			else
			{
				mt = tinfo->get_main_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			for(int32_t j = 0; j < m_argid; j++)
			{
				mt = mt->get_parent_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			m_tstr = mt->get_comm();
			return (uint8_t*)m_tstr.c_str();
		}
	case TYPE_LOGINSHELLID:
		{
			sinsp_threadinfo* mt = NULL;
			int64_t* res = NULL;

			if(tinfo->is_main_thread())
			{
				mt = tinfo;
			}
			else
			{
				mt = tinfo->get_main_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			for(; mt != NULL; mt = mt->get_parent_thread())
			{
				size_t len = mt->m_comm.size();

				if(len >= 2 && mt->m_comm[len - 2] == 's' && mt->m_comm[len - 1] == 'h')
				{
					res = &mt->m_pid;
				}
			}

			return (uint8_t*)res;
		}
	case TYPE_DURATION:
		if(tinfo->m_clone_ts != 0)
		{
			m_s64val = evt->get_ts() - tinfo->m_clone_ts;
			ASSERT(m_s64val > 0);
			return (uint8_t*)&m_s64val;
		}
		else
		{
			return NULL;
		}
	case TYPE_FDOPENCOUNT:
		m_u64val = tinfo->get_fd_opencount();
		return (uint8_t*)&m_u64val;
	case TYPE_FDLIMIT:
		m_s64val = tinfo->get_fd_limit();
		return (uint8_t*)&m_s64val;
	case TYPE_FDUSAGE:
		m_dval = tinfo->get_fd_usage_pct_d();
		return (uint8_t*)&m_dval;
	case TYPE_VMSIZE:
		m_u64val = tinfo->m_vmsize_kb;
		return (uint8_t*)&m_u64val;
	case TYPE_VMRSS:
		m_u64val = tinfo->m_vmrss_kb;
		return (uint8_t*)&m_u64val;
	case TYPE_VMSWAP:
		m_u64val = tinfo->m_vmswap_kb;
		return (uint8_t*)&m_u64val;
	case TYPE_THREAD_VMSIZE:
		if(tinfo->is_main_thread())
		{
			m_u64val = tinfo->m_vmsize_kb;
		}
		else
		{
			m_u64val = 0;
		}

		return (uint8_t*)&m_u64val;
	case TYPE_THREAD_VMRSS:
		if(tinfo->is_main_thread())
		{
			m_u64val = tinfo->m_vmrss_kb;
		}
		else
		{
			m_u64val = 0;
		}

		return (uint8_t*)&m_u64val;
	case TYPE_PFMAJOR:
		m_u64val = tinfo->m_pfmajor;
		return (uint8_t*)&m_u64val;
	case TYPE_PFMINOR:
		m_u64val = tinfo->m_pfminor;
		return (uint8_t*)&m_u64val;
	case TYPE_CGROUPS:
		{
			m_tstr.clear();

			uint32_t j;
			uint32_t nargs = (uint32_t)tinfo->m_cgroups.size();

			if(nargs == 0)
			{
				return NULL;
			}
			
			for(j = 0; j < nargs; j++)
			{
				m_tstr += tinfo->m_cgroups[j].first;
				m_tstr += "=";
				m_tstr += tinfo->m_cgroups[j].second;				
				if(j < nargs - 1)
				{
					m_tstr += ' ';
				}
			}

			return (uint8_t*)m_tstr.c_str();
		}
	case TYPE_CGROUP:
		{
			uint32_t nargs = (uint32_t)tinfo->m_cgroups.size();

			if(nargs == 0)
			{
				return NULL;
			}
			
			for(uint32_t j = 0; j < nargs; j++)
			{
				if(tinfo->m_cgroups[j].first == m_argname)
				{
					m_tstr = tinfo->m_cgroups[j].second;
					return (uint8_t*)m_tstr.c_str();					
				}
			}

			return NULL;
		}
	case TYPE_VTID:
		if(tinfo->m_vtid == -1)
		{
			return NULL;
		}

		m_u64val = tinfo->m_vtid;
		return (uint8_t*)&m_u64val;
	case TYPE_VPID:
		if(tinfo->m_vpid == -1)
		{
			return NULL;
		}

		m_u64val = tinfo->m_vpid;
		return (uint8_t*)&m_u64val;
/*
	case TYPE_PROC_CPU:
		{
			uint16_t etype = evt->get_type();

			if(etype == PPME_PROCINFO_E)
			{
				double thval;
				uint64_t tcpu;

				sinsp_evt_param* parinfo = evt->get_param(0);
				tcpu = *(uint64_t*)parinfo->m_val;

				parinfo = evt->get_param(1);
				tcpu += *(uint64_t*)parinfo->m_val;

				if(tinfo->m_last_t_tot_cpu != 0)
				{
					uint64_t deltaval = tcpu - tinfo->m_last_t_tot_cpu;
					thval = (double)deltaval;// / (ONE_SECOND_IN_NS / 100);
					if(thval > 100)
					{
						thval = 100;
					}
				}
				else
				{
					thval = 0;
				}

				tinfo->m_last_t_tot_cpu = tcpu;

				uint64_t ets = evt->get_ts();
				sinsp_threadinfo* mt = tinfo->get_main_thread();

				if(ets != mt->m_last_mt_cpu_ts)
				{
					mt->m_last_mt_tot_cpu = 0;
					mt->m_last_mt_cpu_ts = ets;
				}

				mt->m_last_mt_tot_cpu += thval;
				m_dval = mt->m_last_mt_tot_cpu;

				return (uint8_t*)&m_dval;
			}

			return NULL;
		}
*/
	case TYPE_THREAD_CPU:
		{
			return extract_thread_cpu(evt, tinfo, true, true);
		}
	case TYPE_THREAD_CPU_USER:
		{
			return extract_thread_cpu(evt, tinfo, true, false);
		}
	case TYPE_THREAD_CPU_SYSTEM:
		{
			return extract_thread_cpu(evt, tinfo, false, true);
		}
	default:
		ASSERT(false);
		return NULL;
	}
}

bool sinsp_filter_check_thread::compare_full_apid(sinsp_evt *evt)
{
	bool res;
	uint32_t j;

	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL)
	{
		return false;
	}

	sinsp_threadinfo* mt = NULL;

	if(tinfo->is_main_thread())
	{
		mt = tinfo;
	}
	else
	{
		mt = tinfo->get_main_thread();

		if(mt == NULL)
		{
			return false;
		}
	}

	//
	// No id specified, search in all of the ancestors
	//
	for(j = 0; mt != NULL; mt = mt->get_parent_thread(), j++)
	{
		if(j > 0)
		{
			res = flt_compare(m_cmpop,
				PT_PID, 
				&mt->m_pid, 
				&m_val_storage[0]);

			if(res == true)
			{
				return true;
			}
		}
	}

	return false;
}

bool sinsp_filter_check_thread::compare_full_aname(sinsp_evt *evt)
{
	bool res;
	uint32_t j;

	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL)
	{
		return false;
	}

	sinsp_threadinfo* mt = NULL;

	if(tinfo->is_main_thread())
	{
		mt = tinfo;
	}
	else
	{
		mt = tinfo->get_main_thread();

		if(mt == NULL)
		{
			return false;
		}
	}

	//
	// No id specified, search in all of the ancestors
	//
	for(j = 0; mt != NULL; mt = mt->get_parent_thread(), j++)
	{
		if(j > 0)
		{
			res = flt_compare(m_cmpop,
				PT_CHARBUF, 
				(void*)mt->m_comm.c_str(), 
				&m_val_storage[0]);

			if(res == true)
			{
				return true;
			}
		}
	}

	return false;
}

bool sinsp_filter_check_thread::compare(sinsp_evt *evt)
{
	if(m_field_id == TYPE_APID)
	{
		if(m_argid == -1)
		{
			return compare_full_apid(evt);
		}
	}
	else if(m_field_id == TYPE_ANAME)
	{
		if(m_argid == -1)
		{
			return compare_full_aname(evt);
		}
	}

	return sinsp_filter_check::compare(evt);
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_event implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_event_fields[] =
{
	{PT_UINT64, EPF_NONE, PF_ID, "evt.num", "event number."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.time", "event timestamp as a time string that includes the nanosecond part."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.time.s", "event timestamp as a time string with no nanoseconds."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.datetime", "event timestamp as a time string that includes the date."},
	{PT_ABSTIME, EPF_NONE, PF_DEC, "evt.rawtime", "absolute event timestamp, i.e. nanoseconds from epoch."},
	{PT_ABSTIME, EPF_NONE, PF_DEC, "evt.rawtime.s", "integer part of the event timestamp (e.g. seconds since epoch)."},
	{PT_ABSTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.rawtime.ns", "fractional part of the absolute event timestamp."},
	{PT_RELTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.reltime", "number of nanoseconds from the beginning of the capture."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.reltime.s", "number of seconds from the beginning of the capture."},
	{PT_RELTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.reltime.ns", "fractional part (in ns) of the time from the beginning of the capture."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.latency", "delta between an exit event and the correspondent enter event, in nanoseconds."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.latency.s", "integer part of the event latency delta."},
	{PT_RELTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.latency.ns", "fractional part of the event latency delta."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.deltatime", "delta between this event and the previous event, in nanoseconds."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.deltatime.s", "integer part of the delta between this event and the previous event."},
	{PT_RELTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.deltatime.ns", "fractional part of the delta between this event and the previous event."},
	{PT_CHARBUF, EPF_PRINT_ONLY, PF_DIR, "evt.dir", "event direction can be either '>' for enter events or '<' for exit events."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.type", "The name of the event (e.g. 'open')."},
	{PT_UINT32, EPF_NONE, PF_NA, "evt.type.is", "allows one to specify an event type, and returns 1 for events that are of that type. For example, evt.type.is.open returns 1 for open events, 0 for any other event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "syscall.type", "For system call events, the name of the system call (e.g. 'open'). Unset for other events (e.g. switch or sysdig internal events). Use this field instead of evt.type if you need to make sure that the filtered/printed value is actually a system call."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.category", "The event category. Example values are 'file' (for file operations like open and close), 'net' (for network operations like socket and bind), memory (for things like brk or mmap), and so on."},
	{PT_INT16, EPF_NONE, PF_ID, "evt.cpu", "number of the CPU where this event happened."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.args", "all the event arguments, aggregated into a single string."},
	{PT_CHARBUF, EPF_REQUIRES_ARGUMENT, PF_NA, "evt.arg", "one of the event arguments specified by name or by number. Some events (e.g. return codes or FDs) will be converted into a text representation when possible. E.g. 'evt.arg.fd' or 'evt.arg[0]'."},
	{PT_DYN, EPF_REQUIRES_ARGUMENT, PF_NA, "evt.rawarg", "one of the event arguments specified by name. E.g. 'evt.rawarg.fd'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.info", "for most events, this field returns the same value as evt.args. However, for some events (like writes to /dev/log) it provides higher level information coming from decoding the arguments."},
	{PT_BYTEBUF, EPF_NONE, PF_NA, "evt.buffer", "the binary data buffer for events that have one, like read(), recvfrom(), etc. Use this field in filters with 'contains' to search into I/O data buffers."},
	{PT_UINT64, EPF_NONE, PF_DEC, "evt.buflen", "the length of the binary data buffer for events that have one, like read(), recvfrom(), etc."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "evt.res", "event return value, as a string. If the event failed, the result is an error code string (e.g. 'ENOENT'), otherwise the result is the string 'SUCCESS'."},
	{PT_INT64, EPF_NONE, PF_DEC, "evt.rawres", "event return value, as a number (e.g. -2). Useful for range comparisons."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.failed", "'true' for events that returned an error status."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_io", "'true' for events that read or write to FDs, like read(), send, recvfrom(), etc."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_io_read", "'true' for events that read from FDs, like read(), recv(), recvfrom(), etc."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_io_write", "'true' for events that write to FDs, like write(), send(), etc."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.io_dir", "'r' for events that read from FDs, like read(); 'w' for events that write to FDs, like write()."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_wait", "'true' for events that make the thread wait, e.g. sleep(), select(), poll()."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.wait_latency", "for events that make the thread wait (e.g. sleep(), select(), poll()), this is the time spent waiting for the event to return, in nanoseconds."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_syslog", "'true' for events that are writes to /dev/log."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count", "This filter field always returns 1 and can be used to count events from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error", "This filter field returns 1 for events that returned with an error, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error.file", "This filter field returns 1 for events that returned with an error and are related to file I/O, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error.net", "This filter field returns 1 for events that returned with an error and are related to network I/O, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error.memory", "This filter field returns 1 for events that returned with an error and are related to memory allocation, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error.other", "This filter field returns 1 for events that returned with an error and are related to none of the previous categories, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.exit", "This filter field returns 1 for exit events, and can be used to count single events from inside chisels."},
	{PT_UINT32, EPF_TABLE_ONLY, PF_DEC, "evt.count.procinfo", "This filter field returns 1 for procinfo events generated by process main threads, and can be used to count processes from inside views."},
	{PT_UINT32, EPF_TABLE_ONLY, PF_DEC, "evt.count.threadinfo", "This filter field returns 1 for procinfo events, and can be used to count processes from inside views."},
	{PT_UINT64, EPF_FILTER_ONLY, PF_DEC, "evt.around", "Accepts the event if it's around the specified time interval. The syntax is evt.around[T]=D, where T is the value returned by %evt.rawtime for the event and D is a delta in milliseconds. For example, evt.around[1404996934793590564]=1000 will return the events with timestamp with one second before the timestamp and one second after it, for a total of two seconds of capture."},
	{PT_CHARBUF, EPF_REQUIRES_ARGUMENT, PF_NA, "evt.abspath", "Absolute path calculated from dirfd and name during syscalls like renameat and symlinkat. Use 'evt.abspath.src' or 'evt.abspath.dst' for syscalls that support multiple paths."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.in", "the length of the binary data buffer, but only for input I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.out", "the length of the binary data buffer, but only for output I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.file", "the length of the binary data buffer, but only for file I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.file.in", "the length of the binary data buffer, but only for input file I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.file.out", "the length of the binary data buffer, but only for output file I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.net", "the length of the binary data buffer, but only for network I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.net.in", "the length of the binary data buffer, but only for input network I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.net.out", "the length of the binary data buffer, but only for output network I/O events."},
};

sinsp_filter_check_event::sinsp_filter_check_event()
{
	m_first_ts = 0;
	m_is_compare = false;
	m_info.m_name = "evt";
	m_info.m_fields = sinsp_filter_check_event_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_event_fields) / sizeof(sinsp_filter_check_event_fields[0]);
	m_u64val = 0;
}

sinsp_filter_check* sinsp_filter_check_event::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_event();
}

int32_t sinsp_filter_check_event::extract_arg(string fldname, string val, OUT const struct ppm_param_info** parinfo)
{
	uint32_t parsed_len = 0;

	//
	// 'arg' and 'resarg' are handled in a custom way
	//
	if(val[fldname.size()] == '[')
	{
		if(parinfo != NULL)
		{
			throw sinsp_exception("evt.arg fields must be expressed explicitly");
		}

		parsed_len = (uint32_t)val.find(']');
		string numstr = val.substr(fldname.size() + 1, parsed_len - fldname.size() - 1);

		if(m_field_id == TYPE_AROUND)
		{
			m_u64val = sinsp_numparser::parseu64(numstr);		
		}
		else
		{
			m_argid = sinsp_numparser::parsed32(numstr);
		}

		parsed_len++;
	}
	else if(val[fldname.size()] == '.')
	{
		if(m_field_id == TYPE_AROUND)
		{
			throw sinsp_exception("wrong syntax for evt.around");
		}

		const struct ppm_param_info* pi = 
			sinsp_utils::find_longest_matching_evt_param(val.substr(fldname.size() + 1));

		if(pi == NULL)
		{
			throw sinsp_exception("unknown event argument " + val.substr(fldname.size() + 1));
		}

		m_argname = pi->name;
		parsed_len = (uint32_t)(fldname.size() + strlen(pi->name) + 1);
		m_argid = -1;

		if(parinfo != NULL)
		{
			*parinfo = pi;
		}
	}
	else
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	return parsed_len; 
}

int32_t sinsp_filter_check_event::extract_type(string fldname, string val, OUT const struct ppm_param_info** parinfo)
{
	uint32_t parsed_len = 0;

	if(val[fldname.size()] == '.')
	{
		string itype = val.substr(fldname.size() + 1);

		if(sinsp_numparser::tryparseu32(itype, &m_evtid))
		{
			m_evtid1 = PPM_EVENT_MAX;
			parsed_len = (uint32_t)(fldname.size() + itype.size() + 1);
			return parsed_len; 
		}

		for(uint32_t j = 0; j < PPM_EVENT_MAX; j++)
		{
			const ppm_event_info* ei = &g_infotables.m_event_info[j];

			if(itype == ei->name)
			{
				m_evtid = j;
				m_evtid1 = j + 1;
				parsed_len = (uint32_t)(fldname.size() + strlen(ei->name) + 1);
				break;
			}
		}
	}
	else
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	return parsed_len; 
}

int32_t sinsp_filter_check_event::parse_field_name(const char* str, bool alloc_state)
{
	string val(str);

	//
	// A couple of fields are handled in a custom way
	//
	if(string(val, 0, sizeof("evt.arg") - 1) == "evt.arg" &&
		string(val, 0, sizeof("evt.args") - 1) != "evt.args")
	{
		m_field_id = TYPE_ARGSTR;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("evt.arg", val, NULL);
	}
	else if(string(val, 0, sizeof("evt.rawarg") - 1) == "evt.rawarg")
	{
		m_field_id = TYPE_ARGRAW;
		m_customfield = m_info.m_fields[m_field_id];
		m_field = &m_customfield;

		int32_t res = extract_arg("evt.rawarg", val, &m_arginfo);

		m_customfield.m_type = m_arginfo->type;

		return res;
	}
	else if(string(val, 0, sizeof("evt.around") - 1) == "evt.around")
	{
		m_field_id = TYPE_AROUND;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("evt.around", val, NULL);
	}
	else if(string(val, 0, sizeof("evt.latency") - 1) == "evt.latency" ||
		string(val, 0, sizeof("evt.latency.s") - 1) == "evt.latency.s" ||
		string(val, 0, sizeof("evt.latency.ns") - 1) == "evt.latency.ns")
	{
		//
		// These fields need to store the previuos event type in the thread state
		//
		if(alloc_state)
		{
			m_th_state_id = m_inspector->reserve_thread_memory(sizeof(uint16_t));
		}

		return sinsp_filter_check::parse_field_name(str, alloc_state);
	}
	else if(string(val, 0, sizeof("evt.abspath") - 1) == "evt.abspath")
	{
		m_field_id = TYPE_ABSPATH;
		m_field = &m_info.m_fields[m_field_id];

		if (val == "evt.abspath")
		{
			m_argid = 0;
		}
		else if (val == "evt.abspath.src")
		{
			m_argid = 1;
		}
		else if (val == "evt.abspath.dst")
		{
			m_argid = 2;
		}
		else
		{
			throw sinsp_exception("wrong syntax for evt.abspath");
		}

		return (int32_t)val.size() + 1;
	}
	else if(string(val, 0, sizeof("evt.type.is") - 1) == "evt.type.is")
	{
		m_field_id = TYPE_TYPE_IS;
		m_field = &m_info.m_fields[m_field_id];

		return extract_type("evt.type.is", val, NULL);
	}
	else
	{
		return sinsp_filter_check::parse_field_name(str, alloc_state);
	}
}

void sinsp_filter_check_event::parse_filter_value(const char* str, uint32_t len)
{
	string val(str);

	if(m_field_id == TYPE_ARGRAW)
	{
		//
		// 'rawarg' is handled in a custom way
		//
		ASSERT(m_arginfo != NULL);
		return sinsp_filter_check::string_to_rawval(str, len, m_arginfo->type);
	}
	else if(m_field_id == TYPE_TYPE)
	{
		sinsp_evttables* einfo = m_inspector->get_event_info_tables();
		const struct ppm_event_info* etable = einfo->m_event_info;
		const struct ppm_syscall_desc* stable = einfo->m_syscall_info_table;
		string stype(str, len);

		for(uint32_t j = 0; j < PPM_EVENT_MAX; j++)
		{
			if(stype == etable[j].name)
			{
				return sinsp_filter_check::parse_filter_value(str, len);
			}
		}

		for(uint32_t j = 0; j < PPM_SC_MAX; j++)
		{
			if(stype == stable[j].name)
			{
				return sinsp_filter_check::parse_filter_value(str, len);
			}
		}

		throw sinsp_exception("unknown event type " + stype);
	}
	else if(m_field_id == TYPE_AROUND)
	{
		if(m_cmpop != CO_EQ)
		{
			throw sinsp_exception("evt.around supports only '=' comparison operator");
		}

		sinsp_filter_check::parse_filter_value(str, len);

		m_tsdelta = sinsp_numparser::parseu64(str) * 1000000;

		return;
	}
	else
	{
		return sinsp_filter_check::parse_filter_value(str, len);
	}
}

const filtercheck_field_info* sinsp_filter_check_event::get_field_info()
{
	if(m_field_id == TYPE_ARGRAW)
	{
		return &m_customfield;
	}
	else
	{
		return &m_info.m_fields[m_field_id];
	}
}

int32_t sinsp_filter_check_event::gmt2local(time_t t)
{
	int dt, dir;
	struct tm *gmt, *loc;
	struct tm sgmt;

	if(t == 0)
	{
		t = time(NULL);
	}

	gmt = &sgmt;
	*gmt = *gmtime(&t);
	loc = localtime(&t);

	dt = (loc->tm_hour - gmt->tm_hour) * 60 * 60 + (loc->tm_min - gmt->tm_min) * 60;

	dir = loc->tm_year - gmt->tm_year;
	if(dir == 0)
	{
		dir = loc->tm_yday - gmt->tm_yday;
	}

	dt += dir * 24 * 60 * 60;

	return dt;
}

void sinsp_filter_check_event::ts_to_string(uint64_t ts, OUT string* res, bool date, bool ns)
{
	struct tm *tm;
	time_t Time;
	uint64_t sec = ts / ONE_SECOND_IN_NS;
	uint64_t nsec = ts % ONE_SECOND_IN_NS;
	int32_t thiszone = gmt2local(0);
	int32_t s = (sec + thiszone) % 86400;
	int32_t bufsize = 0;
	char buf[256];

	if(date) 
	{
		Time = (sec + thiszone) - s;
		tm = gmtime (&Time);
		if(!tm)
		{
			bufsize = sprintf(buf, "<date error> ");
		}
		else
		{
			bufsize = sprintf(buf, "%04d-%02d-%02d ",
				   tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday);
		}
	}

	if(ns)
	{
		sprintf(buf + bufsize, "%02d:%02d:%02d.%09u",
				s / 3600, (s % 3600) / 60, s % 60, (unsigned)nsec);
	}
	else
	{
		sprintf(buf + bufsize, "%02d:%02d:%02d",
				s / 3600, (s % 3600) / 60, s % 60);
	}

	*res = buf;
}

uint8_t* extract_argraw(sinsp_evt *evt, OUT uint32_t* len, const char *argname)
{
	const sinsp_evt_param* pi = evt->get_param_value_raw(argname);

	if(pi != NULL)
	{
		*len = pi->m_len;
		return (uint8_t*)pi->m_val;
	}
	else
	{
		return NULL;
	}
}

uint8_t *sinsp_filter_check_event::extract_abspath(sinsp_evt *evt, OUT uint32_t *len)
{
	sinsp_evt_param *parinfo;
	char *path;
	uint32_t pathlen;
	string spath;

	if(evt->m_tinfo == NULL)
	{
		return NULL;
	}

	uint16_t etype = evt->get_type();

	const char *dirfdarg = NULL, *patharg = NULL;
	if (etype == PPME_SYSCALL_RENAMEAT_X)
	{
		if (m_argid == 1)
		{
			dirfdarg = "olddirfd";
			patharg = "oldpath";
		}
		else if (m_argid == 2)
		{
			dirfdarg = "newdirfd";
			patharg = "newpath";
		}
	}
	else if (etype == PPME_SYSCALL_SYMLINKAT_X)
	{
		dirfdarg = "linkdirfd";
		patharg = "linkpath";
	}
	else if (etype == PPME_SYSCALL_OPENAT_E)
	{
		dirfdarg = "dirfd";
		patharg = "name";
	}
	else if (etype == PPME_SYSCALL_LINKAT_E)
	{
		if (m_argid == 1)
		{
			dirfdarg = "olddir";
			patharg = "oldpath";
		}
		else if (m_argid == 2)
		{
			dirfdarg = "newdir";
			patharg = "newpath";
		}
	}
	else if (etype == PPME_SYSCALL_UNLINKAT_E)
	{
		dirfdarg = "dirfd";
		patharg = "name";
	}

	if (!dirfdarg || !patharg)
	{
		return 0;
	}

	int dirfdargidx = -1, pathargidx = -1, idx = 0;
	while (((dirfdargidx < 0) || (pathargidx < 0)) && (idx < (int) evt->get_num_params()))
	{
		const char *name = evt->get_param_name(idx);
		if ((dirfdargidx < 0) && (strcmp(name, dirfdarg) == 0))
		{
			dirfdargidx = idx;
		}
		if ((pathargidx < 0) && (strcmp(name, patharg) == 0))
		{
			pathargidx = idx;
		}
		idx++;
	}

	if ((dirfdargidx < 0) || (pathargidx < 0))
	{
		return 0;
	}

	parinfo = evt->get_param(dirfdargidx);
	ASSERT(parinfo->m_len == sizeof(int64_t));
	int64_t dirfd = *(int64_t *)parinfo->m_val;

	parinfo = evt->get_param(pathargidx);
	path = parinfo->m_val;
	pathlen = parinfo->m_len;

	string sdir;

	bool is_absolute = (path[0] == '/');
	if(is_absolute)
	{
		//
		// The path is absoulte.
		// Some processes (e.g. irqbalance) actually do this: they pass an invalid fd and
		// and bsolute path, and openat succeeds.
		//
		sdir = ".";
	}
	else if(dirfd == PPM_AT_FDCWD)
	{
		sdir = evt->m_tinfo->get_cwd();
	}
	else
	{
		evt->m_fdinfo = evt->m_tinfo->get_fd(dirfd);

		if(evt->m_fdinfo == NULL)
		{
			ASSERT(false);
			sdir = "<UNKNOWN>/";
		}
		else
		{
			if(evt->m_fdinfo->m_name[evt->m_fdinfo->m_name.length()] == '/')
			{
				sdir = evt->m_fdinfo->m_name;
			}
			else
			{
				sdir = evt->m_fdinfo->m_name + '/';
			}
		}
	}

	char fullname[SCAP_MAX_PATH_SIZE];
	sinsp_utils::concatenate_paths(fullname, SCAP_MAX_PATH_SIZE, sdir.c_str(), (uint32_t)sdir.length(), path, pathlen);

	m_strstorage = fullname;

	return (uint8_t*)m_strstorage.c_str();
}

inline uint8_t* sinsp_filter_check_event::extract_buflen(sinsp_evt *evt)
{
	if(evt->get_direction() == SCAP_ED_OUT)
	{
		sinsp_evt_param *parinfo;
		int64_t retval;

		//
		// Extract the return value
		//
		parinfo = evt->get_param(0);
		ASSERT(parinfo->m_len == sizeof(int64_t));
		retval = *(int64_t *)parinfo->m_val;
						
		if(retval >= 0)
		{
			return (uint8_t*)parinfo->m_val;
		}
	}

	return NULL;
}

Json::Value sinsp_filter_check_event::extract_as_js(sinsp_evt *evt, OUT uint32_t* len)
{
	switch(m_field_id)
	{
	case TYPE_TIME:
	case TYPE_TIME_S:
	case TYPE_DATETIME:
		return (Json::Value::Int64)evt->get_ts();

	case TYPE_RAWTS:
	case TYPE_RAWTS_S:
	case TYPE_RAWTS_NS:
	case TYPE_RELTS:
	case TYPE_RELTS_S:
	case TYPE_RELTS_NS:
	case TYPE_LATENCY:
	case TYPE_LATENCY_S:
	case TYPE_LATENCY_NS:
	case TYPE_DELTA:
	case TYPE_DELTA_S:
	case TYPE_DELTA_NS:
		return (Json::Value::Int64)*(uint64_t*)extract(evt, len);
	case TYPE_COUNT:
		m_u32val = 1;
		return m_u32val;

	default:
		return Json::Value::nullRef;
	}

	return Json::Value::nullRef;
}

uint8_t* sinsp_filter_check_event::extract_error_count(sinsp_evt *evt, OUT uint32_t* len)
{
	const sinsp_evt_param* pi = evt->get_param_value_raw("res");

	if(pi != NULL)
	{
		ASSERT(pi->m_len == sizeof(uint64_t));

		int64_t res = *(int64_t*)pi->m_val;
		if(res < 0)
		{
			m_u32val = 1;
			return (uint8_t*)&m_u32val;
		}
		else
		{
			return NULL;
		}
	}

	if((evt->get_flags() & EF_CREATES_FD) && PPME_IS_EXIT(evt->get_type()))
	{
		pi = evt->get_param_value_raw("fd");

		if(pi != NULL)
		{
			ASSERT(pi->m_len == sizeof(uint64_t));

			int64_t res = *(int64_t*)pi->m_val;
			if(res < 0)
			{
				m_u32val = 1;
				return (uint8_t*)&m_u32val;
			}
		}
	}

	return NULL;
}

uint8_t* sinsp_filter_check_event::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	switch(m_field_id)
	{
	case TYPE_TIME:
		ts_to_string(evt->get_ts(), &m_strstorage, false, true);
		return (uint8_t*)m_strstorage.c_str();
	case TYPE_TIME_S:
		ts_to_string(evt->get_ts(), &m_strstorage, false, false);
		return (uint8_t*)m_strstorage.c_str();
	case TYPE_DATETIME:
		ts_to_string(evt->get_ts(), &m_strstorage, true, true);
		return (uint8_t*)m_strstorage.c_str();
	case TYPE_RAWTS:
		return (uint8_t*)&evt->m_pevt->ts;
	case TYPE_RAWTS_S:
		m_u64val = evt->get_ts() / ONE_SECOND_IN_NS;
		return (uint8_t*)&m_u64val;
	case TYPE_RAWTS_NS:
		m_u64val = evt->get_ts() % ONE_SECOND_IN_NS;
		return (uint8_t*)&m_u64val;
	case TYPE_RELTS:
		if(m_first_ts == 0)
		{
			m_first_ts = evt->get_ts();
		}

		m_u64val = evt->get_ts() - m_first_ts;
		return (uint8_t*)&m_u64val;
	case TYPE_RELTS_S:
		if(m_first_ts == 0)
		{
			m_first_ts = evt->get_ts();
		}

		m_u64val = (evt->get_ts() - m_first_ts) / ONE_SECOND_IN_NS;
		return (uint8_t*)&m_u64val;
	case TYPE_RELTS_NS:
		if(m_first_ts == 0)
		{
			m_first_ts = evt->get_ts();
		}

		m_u64val = (evt->get_ts() - m_first_ts) % ONE_SECOND_IN_NS;
		return (uint8_t*)&m_u64val;
	case TYPE_LATENCY:
		{
			m_u64val = 0;

			if(evt->m_tinfo != NULL)
			{
				m_u64val = evt->m_tinfo->m_latency;
			}

			return (uint8_t*)&m_u64val;
		}
	case TYPE_LATENCY_S:
	case TYPE_LATENCY_NS:
		{
			m_u64val = 0;

			if(evt->m_tinfo != NULL)
			{
				uint64_t lat = evt->m_tinfo->m_latency;

				if(m_field_id == TYPE_LATENCY_S)
				{
					m_u64val = lat / 1000000000;
				}
				else
				{
					m_u64val = lat % 1000000000;
				}
			}

			return (uint8_t*)&m_u64val;
		}
	case TYPE_DELTA:
	case TYPE_DELTA_S:
	case TYPE_DELTA_NS:
		{
			if(m_u64val == 0)
			{
				m_u64val = evt->get_ts();
				m_tsdelta = 0;
			}
			else
			{
				uint64_t tts = evt->get_ts();
				
				if(m_field_id == TYPE_DELTA)
				{
					m_tsdelta = tts - m_u64val;
				}
				else if(m_field_id == TYPE_DELTA_S)
				{
					m_tsdelta = (tts - m_u64val) / ONE_SECOND_IN_NS;
				}
				else if(m_field_id == TYPE_DELTA_NS)
				{
					m_tsdelta = (tts - m_u64val) % ONE_SECOND_IN_NS;
				}

				m_u64val = tts;
			}

			return (uint8_t*)&m_tsdelta;
		}
	case TYPE_DIR:
		if(PPME_IS_ENTER(evt->get_type()))
		{
			return (uint8_t*)">";
		}
		else
		{
			return (uint8_t*)"<";
		}
	case TYPE_TYPE:
		{
			uint8_t* evname;
			uint16_t etype = evt->m_pevt->type;

			if(etype == PPME_GENERIC_E || etype == PPME_GENERIC_X)
			{
				sinsp_evt_param *parinfo = evt->get_param(0);
				ASSERT(parinfo->m_len == sizeof(uint16_t));
				uint16_t evid = *(uint16_t *)parinfo->m_val;

				evname = (uint8_t*)g_infotables.m_syscall_info_table[evid].name;
			}
			else
			{
				evname = (uint8_t*)evt->get_name();
			}

			return evname;
		}
		break;
	case TYPE_TYPE_IS:
		{
			uint16_t etype = evt->m_pevt->type;

			if(etype == m_evtid || etype == m_evtid1)
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}

			return (uint8_t*)&m_u32val;
		}
		break;

	case TYPE_SYSCALL_TYPE:
		{
			uint8_t* evname;
			uint16_t etype = evt->m_pevt->type;
			enum ppm_event_flags flags = g_infotables.m_event_info[etype].flags;

			if(etype == PPME_SCHEDSWITCH_6_E || 
				(flags & EC_INTERNAL) || (flags & EF_SKIPPARSERESET))
			{
				return NULL;
			}

			if(etype == PPME_GENERIC_E || etype == PPME_GENERIC_X)
			{
				sinsp_evt_param *parinfo = evt->get_param(0);
				ASSERT(parinfo->m_len == sizeof(uint16_t));
				uint16_t evid = *(uint16_t *)parinfo->m_val;

				evname = (uint8_t*)g_infotables.m_syscall_info_table[evid].name;
			}
			else
			{
				evname = (uint8_t*)evt->get_name();
			}

			return evname;
		}
		break;
	case TYPE_CATEGORY:
		sinsp_evt::category cat;
		evt->get_category(&cat);

		switch(cat.m_category)
		{
		case EC_UNKNOWN:
			m_strstorage = "unknown";
			break;
		case EC_OTHER:
			m_strstorage = "other";
			break;
		case EC_FILE:
			m_strstorage = "file";
			break;
		case EC_NET:
			m_strstorage = "net";
			break;
		case EC_IPC:
			m_strstorage = "IPC";
			break;
		case EC_MEMORY:
			m_strstorage = "memory";
			break;
		case EC_PROCESS:
			m_strstorage = "process";
			break;
		case EC_SLEEP:
			m_strstorage = "sleep";
			break;
		case EC_SYSTEM:
			m_strstorage = "system";
			break;
		case EC_SIGNAL:
			m_strstorage = "signal";
			break;
		case EC_USER:
			m_strstorage = "user";
			break;
		case EC_TIME:
			m_strstorage = "time";
			break;
		case EC_PROCESSING:
			m_strstorage = "processing";
			break;
		case EC_IO_READ:
		case EC_IO_WRITE:
		case EC_IO_OTHER:
		{
			switch(cat.m_subcategory)
			{
			case sinsp_evt::SC_FILE:
				m_strstorage = "file";
				break;
			case sinsp_evt::SC_NET:
				m_strstorage = "net";
				break;
			case sinsp_evt::SC_IPC:
				m_strstorage = "ipc";
				break;
			case sinsp_evt::SC_NONE:
			case sinsp_evt::SC_UNKNOWN:
			case sinsp_evt::SC_OTHER:
				m_strstorage = "unknown";
				break;
			default:
				ASSERT(false);
				m_strstorage = "unknown";
				break;
			}
		}
		break;
		case EC_WAIT:
			m_strstorage = "wait";
			break;
		case EC_SCHEDULER:
			m_strstorage = "scheduler";
			break;
		default:
			m_strstorage = "unknown";
			break;
		}

		return (uint8_t*)m_strstorage.c_str();
	case TYPE_NUMBER:
		return (uint8_t*)&evt->m_evtnum;
	case TYPE_CPU:
		return (uint8_t*)&evt->m_cpuid;
	case TYPE_ARGRAW:
		return extract_argraw(evt, len, m_arginfo->name);
		break;
	case TYPE_ARGSTR:
		{
			const char* resolved_argstr;
			const char* argstr;

			ASSERT(m_inspector != NULL);

			if(m_argid != -1)
			{
				if(m_argid >= (int32_t)evt->m_info->nparams)
				{
					return NULL;
				}

				argstr = evt->get_param_as_str(m_argid, &resolved_argstr, m_inspector->get_buffer_format());
			}
			else
			{
				argstr = evt->get_param_value_str(m_argname.c_str(), &resolved_argstr, m_inspector->get_buffer_format());
			}

			if(resolved_argstr != NULL && resolved_argstr[0] != 0)
			{
				return (uint8_t*)resolved_argstr;
			}
			else
			{
				return (uint8_t*)argstr;
			}
		}
		break;
	case TYPE_INFO:
		{
			sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

			if(fdinfo != NULL && fdinfo->m_callbaks != NULL)
			{
				char* il;
				vector<sinsp_protodecoder*>* cbacks = &(fdinfo->m_callbaks->m_write_callbacks);

				for(auto it = cbacks->begin(); it != cbacks->end(); ++it)
				{
					if((*it)->get_info_line(&il))
					{
						return (uint8_t*)il;
					}
				}
			}
		}
		//
		// NOTE: this falls through to TYPE_ARGSTR, and that's what we want!
		//       Please don't add anything here!
		//
	case TYPE_ARGS:
		{
			if(evt->get_type() == PPME_GENERIC_E || evt->get_type() == PPME_GENERIC_X)
			{
				//
				// Don't print the arguments for generic events: they have only internal use
				//
				return (uint8_t*)"";
			}

			const char* resolved_argstr = NULL;
			const char* argstr = NULL;
			uint32_t nargs = evt->get_num_params();
			m_strstorage.clear();

			for(uint32_t j = 0; j < nargs; j++)
			{
				ASSERT(m_inspector != NULL);

				argstr = evt->get_param_as_str(j, &resolved_argstr, m_inspector->get_buffer_format());

				if(resolved_argstr[0] == 0)
				{
					m_strstorage += evt->get_param_name(j);
					m_strstorage += '=';
					m_strstorage += argstr;
					m_strstorage += " ";
				}
				else
				{
					m_strstorage += evt->get_param_name(j);
					m_strstorage += '=';
					m_strstorage += argstr;
					m_strstorage += string("(") + resolved_argstr + ") ";
				}
			}

			return (uint8_t*)m_strstorage.c_str();
		}
		break;
	case TYPE_BUFFER:
		{
			if(m_is_compare)
			{
				return extract_argraw(evt, len, "data");
			}

			const char* resolved_argstr;
			const char* argstr;
			argstr = evt->get_param_value_str("data", &resolved_argstr, m_inspector->get_buffer_format());
			*len = evt->m_rawbuf_str_len;

			return (uint8_t*)argstr;
		}
	case TYPE_BUFLEN:
		if(evt->m_fdinfo && evt->get_category() & EC_IO_BASE)
		{
			return extract_buflen(evt);
		}
		break;
	case TYPE_RESRAW:
		{
			const sinsp_evt_param* pi = evt->get_param_value_raw("res");

			if(pi != NULL)
			{
				*len = pi->m_len;
				return (uint8_t*)pi->m_val;
			}

			if((evt->get_flags() & EF_CREATES_FD) && PPME_IS_EXIT(evt->get_type()))
			{
				pi = evt->get_param_value_raw("fd");

				if(pi != NULL)
				{
					*len = pi->m_len;
					return (uint8_t*)pi->m_val;
				}
			}

			return NULL;
		}
		break;
	case TYPE_RESSTR:
		{
			const char* resolved_argstr;
			const char* argstr;

			const sinsp_evt_param* pi = evt->get_param_value_raw("res");

			if(pi != NULL)
			{
				ASSERT(pi->m_len == sizeof(int64_t));

				int64_t res = *(int64_t*)pi->m_val;

				if(res >= 0)
				{
					*len = sizeof("SUCCESS");
					return (uint8_t*)"SUCCESS";
				}
				else
				{
					argstr = evt->get_param_value_str("res", &resolved_argstr);
					ASSERT(resolved_argstr != NULL && resolved_argstr[0] != 0);

					if(resolved_argstr != NULL && resolved_argstr[0] != 0)
					{
						return (uint8_t*)resolved_argstr;
					}
					else if(argstr != NULL)
					{
						return (uint8_t*)argstr;
					}
				}
			}
			else
			{
				if((evt->get_flags() & EF_CREATES_FD) && PPME_IS_EXIT(evt->get_type()))
				{
					pi = evt->get_param_value_raw("fd");

					int64_t res = *(int64_t*)pi->m_val;

					if(res >= 0)
					{
						*len = sizeof("SUCCESS");
						return (uint8_t*)"SUCCESS";
					}
					else
					{
						argstr = evt->get_param_value_str("fd", &resolved_argstr);
						ASSERT(resolved_argstr != NULL && resolved_argstr[0] != 0);

						if(resolved_argstr != NULL && resolved_argstr[0] != 0)
						{
							return (uint8_t*)resolved_argstr;
						}
						else if(argstr != NULL)
						{
							return (uint8_t*)argstr;
						}
					}
				}
			}

			return NULL;
		}
		break;
	case TYPE_FAILED:
		{
			m_u32val = 0;
			const sinsp_evt_param* pi = evt->get_param_value_raw("res");

			if(pi != NULL)
			{
				ASSERT(pi->m_len == sizeof(int64_t));
				if(*(int64_t*)pi->m_val < 0)
				{
					m_u32val = 1;
				}
			}
			else if((evt->get_flags() & EF_CREATES_FD) && PPME_IS_EXIT(evt->get_type()))
			{
				pi = evt->get_param_value_raw("fd");

				if(pi != NULL)
				{
					ASSERT(pi->m_len == sizeof(int64_t));
					if(*(int64_t*)pi->m_val < 0)
					{
						m_u32val = 1;
					}
				}
			}

			return (uint8_t*)&m_u32val;
		}
		break;
	case TYPE_ISIO:
		{
			ppm_event_flags eflags = evt->get_flags();
			if(eflags & (EF_READS_FROM_FD | EF_WRITES_TO_FD))
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}
		}

		return (uint8_t*)&m_u32val;
	case TYPE_ISIO_READ:
		{
			ppm_event_flags eflags = evt->get_flags();
			if(eflags & EF_READS_FROM_FD)
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}

			return (uint8_t*)&m_u32val;
		}
	case TYPE_ISIO_WRITE:
		{
			ppm_event_flags eflags = evt->get_flags();
			if(eflags & EF_WRITES_TO_FD)
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}

			return (uint8_t*)&m_u32val;
		}
	case TYPE_IODIR:
		{
			ppm_event_flags eflags = evt->get_flags();
			if(eflags & EF_WRITES_TO_FD)
			{
				m_strstorage = "write";
			}
			else if(eflags & EF_READS_FROM_FD)
			{
				m_strstorage = "read";
			}
			else
			{
				return NULL;
			}

			return (uint8_t*)m_strstorage.c_str();
		}
	case TYPE_ISWAIT:
		{
			ppm_event_flags eflags = evt->get_flags();
			if(eflags & (EF_WAITS))
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}
		}

		return (uint8_t*)&m_u32val;
	case TYPE_WAIT_LATENCY:
		{
			ppm_event_flags eflags = evt->get_flags();
			uint16_t etype = evt->m_pevt->type;

			if(eflags & (EF_WAITS) && PPME_IS_EXIT(etype))
			{
				if(evt->m_tinfo != NULL)
				{
					m_u64val = evt->m_tinfo->m_latency;
				}
				else
				{
					m_u64val = 0;
				}

				return (uint8_t*)&m_u64val;
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_ISSYSLOG:
		{
			m_u32val = 0;

			ppm_event_flags eflags = evt->get_flags();
			if(eflags & EF_WRITES_TO_FD)
			{
				sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

				if(fdinfo != NULL)
				{
					if(fdinfo->m_name.find("/dev/log") != string::npos)
					{
						m_u32val = 1;
					}
				}
			}

			return (uint8_t*)&m_u32val;
		}
	case TYPE_COUNT:
		m_u32val = 1;
		return (uint8_t*)&m_u32val;
	case TYPE_COUNT_ERROR:
		return extract_error_count(evt, len);
	case TYPE_COUNT_ERROR_FILE:
		{
			sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_FILE ||
					fdinfo->m_type == SCAP_FD_DIRECTORY)
				{
					return extract_error_count(evt, len);
				}
			}
			else
			{
				uint16_t etype = evt->get_type();

				if(etype == PPME_SYSCALL_OPEN_X ||
					etype == PPME_SYSCALL_CREAT_X ||
					etype == PPME_SYSCALL_OPENAT_X)
				{
					return extract_error_count(evt, len);
				}
			}

			return NULL;
		}
	case TYPE_COUNT_ERROR_NET:
		{
			sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK ||
					fdinfo->m_type == SCAP_FD_IPV6_SOCK ||
					fdinfo->m_type == SCAP_FD_IPV4_SERVSOCK ||
					fdinfo->m_type == SCAP_FD_IPV6_SERVSOCK ||
					fdinfo->m_type == SCAP_FD_UNIX_SOCK)
				{
					return extract_error_count(evt, len);
				}
			}
			else
			{
				uint16_t etype = evt->get_type();

				if(etype == PPME_SOCKET_ACCEPT_X ||
					etype == PPME_SOCKET_ACCEPT_5_X ||
					etype == PPME_SOCKET_ACCEPT4_X ||
					etype == PPME_SOCKET_ACCEPT4_5_X ||
					etype == PPME_SOCKET_CONNECT_X)
				{
					return extract_error_count(evt, len);
				}
			}

			return NULL;
		}
	case TYPE_COUNT_ERROR_MEMORY:
		{
			if(evt->get_category() == EC_MEMORY)
			{
				return extract_error_count(evt, len);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_COUNT_ERROR_OTHER:
		{
			sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

			if(fdinfo != NULL)
			{
				if(!(fdinfo->m_type == SCAP_FD_FILE ||
					fdinfo->m_type == SCAP_FD_DIRECTORY ||
					fdinfo->m_type == SCAP_FD_IPV4_SOCK ||
					fdinfo->m_type == SCAP_FD_IPV6_SOCK ||
					fdinfo->m_type == SCAP_FD_IPV4_SERVSOCK ||
					fdinfo->m_type == SCAP_FD_IPV6_SERVSOCK ||
					fdinfo->m_type == SCAP_FD_UNIX_SOCK))
				{
					return extract_error_count(evt, len);
				}
			}
			else
			{
				uint16_t etype = evt->get_type();

				if(!(etype == PPME_SYSCALL_OPEN_X ||
					etype == PPME_SYSCALL_CREAT_X ||
					etype == PPME_SYSCALL_OPENAT_X ||
					etype == PPME_SOCKET_ACCEPT_X ||
					etype == PPME_SOCKET_ACCEPT_5_X ||
					etype == PPME_SOCKET_ACCEPT4_X ||
					etype == PPME_SOCKET_ACCEPT4_5_X ||
					etype == PPME_SOCKET_CONNECT_X ||
					evt->get_category() == EC_MEMORY))
				{
					return extract_error_count(evt, len);
				}
			}

			return NULL;
		}
	case TYPE_COUNT_EXIT:
		if(PPME_IS_EXIT(evt->get_type()))
		{
			m_u32val = 1;
			return (uint8_t*)&m_u32val;
		}
		else
		{
			return NULL;
		}
	case TYPE_COUNT_PROCINFO:
		{
			uint16_t etype = evt->get_type();

			if(etype == PPME_PROCINFO_E)
			{
				sinsp_threadinfo* tinfo = evt->get_thread_info();

				if(tinfo != NULL && tinfo->is_main_thread())
				{
					m_u32val = 1;
					return (uint8_t*)&m_u32val;
				}
			}
		}

		break;
	case TYPE_COUNT_THREADINFO:
		{
			uint16_t etype = evt->get_type();

			if(etype == PPME_PROCINFO_E)
			{
				m_u32val = 1;
				return (uint8_t*)&m_u32val;
			}
		}

		break;
	case TYPE_ABSPATH:
		return extract_abspath(evt, len);
	case TYPE_BUFLEN_IN:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_READ)
		{
			return extract_buflen(evt);
		}

		break;
	case TYPE_BUFLEN_OUT:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_WRITE)
		{
			return extract_buflen(evt);
		}

		break;
	case TYPE_BUFLEN_FILE:
		if(evt->m_fdinfo && evt->get_category() & EC_IO_BASE)
		{
			if(evt->m_fdinfo->m_type == SCAP_FD_FILE)
			{
				return extract_buflen(evt);
			}
		}

		break;
	case TYPE_BUFLEN_FILE_IN:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_READ)
		{
			if(evt->m_fdinfo->m_type == SCAP_FD_FILE)
			{
				return extract_buflen(evt);
			}
		}

		break;
	case TYPE_BUFLEN_FILE_OUT:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_WRITE)
		{
			if(evt->m_fdinfo->m_type == SCAP_FD_FILE)
			{
				return extract_buflen(evt);
			}
		}

		break;
	case TYPE_BUFLEN_NET:
		if(evt->m_fdinfo && evt->get_category() & EC_IO_BASE)
		{
			scap_fd_type etype = evt->m_fdinfo->m_type;

			if((etype >= SCAP_FD_IPV4_SOCK && etype <= SCAP_FD_IPV6_SERVSOCK) || etype == SCAP_FD_UNIX_SOCK)
			{
				return extract_buflen(evt);
			}
		}

		break;
	case TYPE_BUFLEN_NET_IN:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_READ)
		{
			scap_fd_type etype = evt->m_fdinfo->m_type;

			if((etype >= SCAP_FD_IPV4_SOCK && etype <= SCAP_FD_IPV6_SERVSOCK) || etype == SCAP_FD_UNIX_SOCK)
			{
				return extract_buflen(evt);
			}
		}

		break;
	case TYPE_BUFLEN_NET_OUT:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_WRITE)
		{
			scap_fd_type etype = evt->m_fdinfo->m_type;

			if((etype >= SCAP_FD_IPV4_SOCK && etype <= SCAP_FD_IPV6_SERVSOCK) || etype == SCAP_FD_UNIX_SOCK)
			{
				return extract_buflen(evt);
			}
		}

		break;
	default:
		ASSERT(false);
		return NULL;
	}

	return NULL;
}

bool sinsp_filter_check_event::compare(sinsp_evt *evt)
{
	bool res;

	m_is_compare = true;

	if(m_field_id == TYPE_ARGRAW)
	{
		uint32_t len;
		uint8_t* extracted_val = extract(evt, &len);

		if(extracted_val == NULL)
		{
			return false;
		}

		ASSERT(m_arginfo != NULL);

		res = flt_compare(m_cmpop,
			m_arginfo->type, 
			extracted_val, 
			&m_val_storage[0]);
	}
	else if(m_field_id == TYPE_AROUND)
	{
		uint64_t ts = evt->get_ts();
		uint64_t t1 = ts - m_tsdelta;
		uint64_t t2 = ts + m_tsdelta;

		bool res1 = flt_compare(CO_GE,
			PT_UINT64,
			&m_u64val,
			&t1);

		bool res2 = flt_compare(CO_LE,
			PT_UINT64,
			&m_u64val,
			&t2);

		return res1 && res2;
	}
	else
	{
		res = sinsp_filter_check::compare(evt);
	}

	m_is_compare = false;

	return res;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_user implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_user_fields[] =
{
	{PT_UINT32, EPF_NONE, PF_ID, "user.uid", "user ID."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "user.name", "user name."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "user.homedir", "home directory of the user."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "user.shell", "user's shell."},
};

sinsp_filter_check_user::sinsp_filter_check_user()
{
	m_info.m_name = "user";
	m_info.m_fields = sinsp_filter_check_user_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_user_fields) / sizeof(sinsp_filter_check_user_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_user::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_user();
}

uint8_t* sinsp_filter_check_user::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();
	scap_userinfo* uinfo;

	if(tinfo == NULL)
	{
		return NULL;
	}

	if(m_field_id != TYPE_UID)
	{
		unordered_map<uint32_t, scap_userinfo*>::const_iterator it;

		ASSERT(m_inspector != NULL);
		const unordered_map<uint32_t, scap_userinfo*>* userlist = m_inspector->get_userlist();

		if(tinfo->m_uid == 0xffffffff)
		{
			return NULL;
		}

		it = userlist->find(tinfo->m_uid);
		if(it == userlist->end())
		{
			return NULL;
		}

		uinfo = it->second;
		ASSERT(uinfo != NULL);
	}

	switch(m_field_id)
	{
	case TYPE_UID:
		return (uint8_t*)&tinfo->m_uid;
	case TYPE_NAME:
		return (uint8_t*)uinfo->name;
	case TYPE_HOMEDIR:
		return (uint8_t*)uinfo->homedir;
	case TYPE_SHELL:
		return (uint8_t*) uinfo->shell;
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_group implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_group_fields[] =
{
	{PT_UINT64, EPF_NONE, PF_ID, "group.gid", "group ID."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "group.name", "group name."},
};

sinsp_filter_check_group::sinsp_filter_check_group()
{
	m_info.m_name = "group";
	m_info.m_fields = sinsp_filter_check_group_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_group_fields) / sizeof(sinsp_filter_check_group_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_group::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_group();
}

uint8_t* sinsp_filter_check_group::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL)
	{
		return NULL;
	}

	switch(m_field_id)
	{
	case TYPE_GID:
		return (uint8_t*)&tinfo->m_gid;
	case TYPE_NAME:
		{
			unordered_map<uint32_t, scap_groupinfo*>::iterator it;

			ASSERT(m_inspector != NULL);
			unordered_map<uint32_t, scap_groupinfo*>* grouplist = 
				(unordered_map<uint32_t, scap_groupinfo*>*)m_inspector->get_grouplist();
			ASSERT(grouplist->size() != 0);

			if(tinfo->m_gid == 0xffffffff)
			{
				return NULL;
			}

			it = grouplist->find(tinfo->m_gid);
			if(it == grouplist->end())
			{
				ASSERT(false);
				return NULL;
			}

			scap_groupinfo* ginfo = it->second;
			ASSERT(ginfo != NULL);

			return (uint8_t*)ginfo->name;
		}
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// rawstring_check implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info rawstring_check_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_NA, "NA", "INTERNAL."},
};

rawstring_check::rawstring_check(string text)
{
	m_field = rawstring_check_fields;
	m_field_id = 0;
	set_text(text);
}

sinsp_filter_check* rawstring_check::allocate_new()
{
	ASSERT(false);
	return NULL;
}

void rawstring_check::set_text(string text)
{
	m_text_len = (uint32_t)text.size();
	m_text = text;
}

int32_t rawstring_check::parse_field_name(const char* str, bool alloc_state)
{
	ASSERT(false);
	return -1;
}

void rawstring_check::parse_filter_value(const char* str, uint32_t len)
{
	ASSERT(false);
}

uint8_t* rawstring_check::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	*len = m_text_len;
	return (uint8_t*)m_text.c_str();
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_syslog implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_syslog_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_NA, "syslog.facility.str", "facility as a string."},
	{PT_UINT32, EPF_NONE, PF_DEC, "syslog.facility", "facility as a number (0-23)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "syslog.severity.str", "severity as a string. Can have one of these values: emerg, alert, crit, err, warn, notice, info, debug"},
	{PT_UINT32, EPF_NONE, PF_DEC, "syslog.severity", "severity as a number (0-7)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "syslog.message", "message sent to syslog."},
};

sinsp_filter_check_syslog::sinsp_filter_check_syslog()
{
	m_info.m_name = "syslog";
	m_info.m_fields = sinsp_filter_check_syslog_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_syslog_fields) / sizeof(sinsp_filter_check_syslog_fields[0]);
	m_decoder = NULL;
}

sinsp_filter_check* sinsp_filter_check_syslog::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_syslog();
}

int32_t sinsp_filter_check_syslog::parse_field_name(const char* str, bool alloc_state)
{
	int32_t res = sinsp_filter_check::parse_field_name(str, alloc_state);
	if(res != -1)
	{
		m_decoder = (sinsp_decoder_syslog*)m_inspector->require_protodecoder("syslog");
	}

	return res;
}

uint8_t* sinsp_filter_check_syslog::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	ASSERT(m_decoder != NULL);
	if(!m_decoder->is_data_valid())
	{
		return NULL;
	}

	switch(m_field_id)
	{
	case TYPE_FACILITY:
		return (uint8_t*)&m_decoder->m_facility;
	case TYPE_FACILITY_STR:
		return (uint8_t*)m_decoder->get_facility_str();
	case TYPE_SEVERITY:
		return (uint8_t*)&m_decoder->m_severity;
	case TYPE_SEVERITY_STR:
		return (uint8_t*)m_decoder->get_severity_str();
	case TYPE_MESSAGE:
		return (uint8_t*)m_decoder->m_msg.c_str();
	default:
		ASSERT(false);
		return NULL;
	}
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_container implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_container_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.id", "the container id."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.name", "the container name."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.image", "the container image."}
};

sinsp_filter_check_container::sinsp_filter_check_container()
{
	m_info.m_name = "container";
	m_info.m_fields = sinsp_filter_check_container_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_container_fields) / sizeof(sinsp_filter_check_container_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_container::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_container();
}

uint8_t* sinsp_filter_check_container::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();
	if(tinfo == NULL)
	{
		return NULL;
	}

	switch(m_field_id)
	{
	case TYPE_CONTAINER_ID:
		if(tinfo->m_container_id.empty())
		{
			m_tstr = "host";
		}
		else
		{
			m_tstr = tinfo->m_container_id;
		}
		
		return (uint8_t*)m_tstr.c_str();
	case TYPE_CONTAINER_NAME:
		if(tinfo->m_container_id.empty())
		{
			m_tstr = "host";
		}
		else
		{
			sinsp_container_info container_info;
			bool found = m_inspector->m_container_manager.get_container(tinfo->m_container_id, &container_info);
			if(!found)
			{
				return NULL;
			}

			if(container_info.m_name.empty())
			{
				return NULL;
			}

			m_tstr = container_info.m_name;
		}

		return (uint8_t*)m_tstr.c_str();
	case TYPE_CONTAINER_IMAGE:
		if(tinfo->m_container_id.empty())
		{
			return NULL;
		}
		else
		{
			sinsp_container_info container_info;
			bool found = m_inspector->m_container_manager.get_container(tinfo->m_container_id, &container_info);
			if(!found)
			{
				return NULL;
			}

			if(container_info.m_image.empty())
			{
				return NULL;
			}

			m_tstr = container_info.m_image;
		}

		return (uint8_t*)m_tstr.c_str();
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_reference implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_filter_check_reference::sinsp_filter_check_reference()
{
	m_info.m_name = "<NA>";
	m_info.m_fields = &m_finfo;
	m_info.m_nfields = 1;
	m_info.m_flags = 0;
	m_finfo.m_print_format = PF_DEC;
	m_field = &m_finfo;
}

sinsp_filter_check* sinsp_filter_check_reference::allocate_new()
{
	ASSERT(false);
	return NULL;
}

int32_t sinsp_filter_check_reference::parse_field_name(const char* str, bool alloc_state)
{
	ASSERT(false);
	return -1;
}

void sinsp_filter_check_reference::parse_filter_value(const char* str, uint32_t len)
{
	ASSERT(false);
}

uint8_t* sinsp_filter_check_reference::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	*len = m_len;
	return m_val;
}

//
// convert a number into a byte representation.
// E.g. 1230 becomes 1.23K
//
char* sinsp_filter_check_reference::format_bytes(double val, uint32_t str_len, bool is_int)
{
	char* pr_fmt;

	if(is_int)
	{
		pr_fmt = (char*)"%*.0lf%c";
	}
	else
	{
		pr_fmt = (char*)"%*.2lf%c";
	}

	if(val > (1024LL * 1024 * 1024 * 1024 * 1024))
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024LL * 1024 * 1024 * 1024 * 1024), 'P');
	}
	else if(val > (1024LL * 1024 * 1024 * 1024))
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024LL * 1024 * 1024 * 1024), 'T');
	}
	else if(val > (1024LL * 1024 * 1024))
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024LL * 1024 * 1024), 'G');
	}
	else if(val > (1024 * 1024))
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024 * 1024), 'M');
	}
	else if(val > 1024)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024), 'K');
	}
	else
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len, val, 0);
	}

	uint32_t len = (uint32_t)strlen(m_getpropertystr_storage);

	if(len > str_len)
	{
		memmove(m_getpropertystr_storage, 
			m_getpropertystr_storage + len - str_len, 
			str_len + 1); // include trailing \0
	}

	return m_getpropertystr_storage;
}

//
// convert a nanosecond time interval into a s.ns representation.
// E.g. 1100000000 becomes 1.1s
//
#define ONE_MILLISECOND_IN_NS 1000000
#define ONE_MICROSECOND_IN_NS 1000

char* sinsp_filter_check_reference::format_time(uint64_t val, uint32_t str_len)
{
	if(val >= ONE_SECOND_IN_NS)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%u.%02us", (unsigned int)(val / ONE_SECOND_IN_NS), (unsigned int)((val % ONE_SECOND_IN_NS) / 10000000));
	}
	else if(val >= ONE_SECOND_IN_NS / 100)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%ums", (unsigned int)(val / (ONE_SECOND_IN_NS / 1000)));
	}
	else if(val >= ONE_SECOND_IN_NS / 1000)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%u.%02ums", (unsigned int)(val / (ONE_SECOND_IN_NS / 1000)), (unsigned int)((val % ONE_MILLISECOND_IN_NS) / 10000));
	}
	else if(val >= ONE_SECOND_IN_NS / 100000)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%uus", (unsigned int)(val / (ONE_SECOND_IN_NS / 1000000)));
	}
	else if(val >= ONE_SECOND_IN_NS / 1000000)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%u.%02uus", (unsigned int)(val / (ONE_SECOND_IN_NS / 1000000)), (unsigned int)((val % ONE_MICROSECOND_IN_NS) / 10));
	}
	else
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%uns", (unsigned int)val);
	}

	uint32_t reslen = (uint32_t)strlen(m_getpropertystr_storage);
	if(reslen < str_len)
	{
		uint32_t padding_size = str_len - reslen;

		memmove(m_getpropertystr_storage + padding_size, 
			m_getpropertystr_storage,
			str_len + 1);

		for(uint32_t j = 0; j < padding_size; j++)
		{
			m_getpropertystr_storage[j] = ' ';
		}
	}

	return m_getpropertystr_storage;
}

char* sinsp_filter_check_reference::print_double(uint8_t* rawval, uint32_t str_len)
{
	double val;

	switch(m_field->m_type)
	{
	case PT_INT8:
		val = (double)*(int8_t*)rawval;
		break;
	case PT_INT16:
		val = (double)*(int16_t*)rawval;
		break;
	case PT_INT32:
		val = (double)*(int32_t*)rawval;
		break;
	case PT_INT64:
		val = (double)*(int64_t*)rawval;
		break;
	case PT_UINT8:
		val = (double)*(uint8_t*)rawval;
		break;
	case PT_UINT16:
		val = (double)*(uint16_t*)rawval;
		break;
	case PT_UINT32:
		val = (double)*(uint32_t*)rawval;
		break;
	case PT_UINT64:
		val = (double)*(uint64_t*)rawval;
		break;
	default:
		ASSERT(false);
		val = 0;
		break;
	}

	if(m_cnt > 1)
	{
		val /= m_cnt;
	}

	if(m_print_format == PF_ID)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%*lf", str_len, val);
		return m_getpropertystr_storage;
	}
	else
	{
		return format_bytes(val, str_len, false);
	}

}

char* sinsp_filter_check_reference::print_int(uint8_t* rawval, uint32_t str_len)
{
	int64_t val;

	switch(m_field->m_type)
	{
	case PT_INT8:
		val = (int64_t)*(int8_t*)rawval;
		break;
	case PT_INT16:
		val = (int64_t)*(int16_t*)rawval;
		break;
	case PT_INT32:
		val = (int64_t)*(int32_t*)rawval;
		break;
	case PT_INT64:
		val = (int64_t)*(int64_t*)rawval;
		break;
	case PT_UINT8:
		val = (int64_t)*(uint8_t*)rawval;
		break;
	case PT_UINT16:
		val = (int64_t)*(uint16_t*)rawval;
		break;
	case PT_UINT32:
		val = (int64_t)*(uint32_t*)rawval;
		break;
	case PT_UINT64:
		val = (int64_t)*(uint64_t*)rawval;
		break;
	default:
		ASSERT(false);
		val = 0;
		break;
	}

	if(m_cnt > 1)
	{
		val /= (int64_t)m_cnt;
	}

	if(m_print_format == PF_ID)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%*" PRId64, str_len, val);
		return m_getpropertystr_storage;
	}
	else
	{
		return format_bytes((double)val, str_len, true);
	}

}

char* sinsp_filter_check_reference::tostring_nice(sinsp_evt* evt, 
												  uint32_t str_len,
												  uint64_t time_delta)
{
	uint32_t len;
	uint8_t* rawval = extract(evt, &len);

	if(rawval == NULL)
	{
		return NULL;
	}

	if(time_delta != 0)
	{
		m_cnt = (double)time_delta / ONE_SECOND_IN_NS;
	}

	if(m_field->m_type >= PT_INT8 && m_field->m_type <= PT_UINT64)
	{
		if(m_print_format == PF_ID || m_cnt == 1 || m_cnt == 0)
		{
			return print_int(rawval, str_len);
		}
		else
		{
			return print_double(rawval, str_len);
		}
	}
	else if(m_field->m_type == PT_RELTIME)
	{
		double val = (double)*(uint64_t*)rawval;

		if(m_cnt > 1)
		{
			val /= m_cnt;
		}

		return format_time((int64_t)val, str_len);
	}
	else if(m_field->m_type == PT_DOUBLE)
	{
		double dval = (double)*(double*)rawval;
			
		if(m_cnt > 1)
		{
			dval /= m_cnt;
		}

		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%*.2lf", str_len, dval);
		return m_getpropertystr_storage;
	}
	else
	{
		return rawval_to_string(rawval, m_field, len);
	}
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_utils implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_utils_fields[] =
{
	{PT_UINT64, EPF_NONE, PF_ID, "util.cnt", "incremental counter."},
};

sinsp_filter_check_utils::sinsp_filter_check_utils()
{
	m_info.m_name = "util";
	m_info.m_fields = sinsp_filter_check_utils_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_utils_fields) / sizeof(sinsp_filter_check_utils_fields[0]);
	m_info.m_flags = filter_check_info::FL_HIDDEN;
	m_cnt = 0;
}

sinsp_filter_check* sinsp_filter_check_utils::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_utils();
}

uint8_t* sinsp_filter_check_utils::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	switch(m_field_id)
	{
	case TYPE_CNT:
		m_cnt++;
		return (uint8_t*)&m_cnt;
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_fdlist implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_fdlist_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_ID, "fdlist.nums", "for poll events, this is a comma-separated list of the FD numbers in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fdlist.names", "for poll events, this is a comma-separated list of the FD names in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fdlist.cips", "for poll events, this is a comma-separated list of the client IP addresses in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fdlist.sips", "for poll events, this is a comma-separated list of the server IP addresses in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "fdlist.cports", "for TCP/UDP FDs, for poll events, this is a comma-separated list of the client TCP/UDP ports in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "fdlist.sports", "for poll events, this is a comma-separated list of the server TCP/UDP ports in the 'fds' argument, returned as a string."},
};

sinsp_filter_check_fdlist::sinsp_filter_check_fdlist()
{
	m_info.m_name = "fdlist";
	m_info.m_fields = sinsp_filter_check_fdlist_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_fdlist_fields) / sizeof(sinsp_filter_check_fdlist_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_fdlist::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_fdlist();
}

int32_t sinsp_filter_check_fdlist::parse_field_name(const char* str, bool alloc_state)
{
	return sinsp_filter_check::parse_field_name(str, alloc_state);
}

uint8_t* sinsp_filter_check_fdlist::extract(sinsp_evt *evt, OUT uint32_t* len)
{
	ASSERT(evt);
	sinsp_evt_param *parinfo;

	uint16_t etype = evt->get_type();

	if(etype == PPME_SYSCALL_POLL_E)
	{
		parinfo = evt->get_param(0);
	}
	else if(etype == PPME_SYSCALL_POLL_X)
	{
		parinfo = evt->get_param(1);
	}
	else
	{
		return NULL;
	}

	uint32_t j = 0;
	char* payload = parinfo->m_val;
	uint16_t nfds = *(uint16_t *)payload;
	uint32_t pos = 2;
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	m_strval.clear();

	for(j = 0; j < nfds; j++)
	{
		bool add_comma = true;
		int64_t fd = *(int64_t *)(payload + pos);

		sinsp_fdinfo_t *fdinfo = tinfo->get_fd(fd);

		switch(m_field_id)
		{
		case TYPE_FDNUMS:
		{
			m_strval += to_string(fd);
		}
		break;
		case TYPE_FDNAMES:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_name != "")
				{
					m_strval += fdinfo->m_name;
				}
				else
				{
					m_strval += "<NA>";
				}
			}
			else
			{
				m_strval += "<NA>";
			}
		}
		break;
		case TYPE_CLIENTIPS:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK)
				{
					inet_ntop(AF_INET, &fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SOCK)
				{
					inet_ntop(AF_INET6, fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
			}

			add_comma = false;
		}
		break;
		case TYPE_SERVERIPS:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK)
				{
					inet_ntop(AF_INET, &fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SOCK)
				{
					inet_ntop(AF_INET6, fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV4_SERVSOCK)
				{
					inet_ntop(AF_INET, &fdinfo->m_sockinfo.m_ipv4serverinfo.m_ip, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SERVSOCK)
				{
					inet_ntop(AF_INET, &fdinfo->m_sockinfo.m_ipv6serverinfo.m_ip, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
			}

			add_comma = false;
		}
		break;
		case TYPE_CLIENTPORTS:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK)
				{
					m_strval += to_string(fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport);
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SOCK)
				{
					m_strval += to_string(fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport);
					break;
				}
			}

			add_comma = false;
		}
		case TYPE_SERVERPORTS:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK)
				{
					m_strval += to_string(fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport);
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SOCK)
				{
					m_strval += to_string(fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport);
					break;
				}
			}

			add_comma = false;
		}
		break;
		default:
			ASSERT(false);
		}

		if(j < nfds && add_comma)
		{
			m_strval += ",";
		}

		pos += 10;
	}

	if(m_strval.size() != 0)
	{
		if(m_strval.back() == ',')
		{
			m_strval = m_strval.substr(0, m_strval.size() - 1);
		}

		return (uint8_t*)m_strval.c_str();
	}
	else
	{
		return NULL;
	}
}

#endif // HAS_FILTERING
