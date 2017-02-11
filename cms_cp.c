/*
UPnP control point sample for CMS(ConfigurationManagement Service)
	-- by larkguo@gmail.com

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.


1.Architecture:
  cms_cp ==command==> libupnp   (CtrlPointProcessCommand)
  cms_cp <==notify==  libupnp	(CtrlPointCallbackEventHandler	)

2.Requires:
  libupnp-1.6.21

3.Compile:(assumed that libupnp are installed in /usr/local)
	gcc -I/usr/local/include -I/usr/local/include/upnp -L/usr/local/lib cms_cp.c \
	-o cms_cp -g -lupnp -lthreadutil -lixml -lpthread

4.Run:
	export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
	./cms_cp 

5.Valgrind
	valgrind --error-limit=no --tool=memcheck  --leak-check=full  ./cms_cp
	valgrind --error-limit=no --tool=helgrind  ./cms_cp

*/
#include "cms_cp.h"

UpnpClient_Handle g_cpHandle = -1;

/* Timeout to request during subscriptions */
int g_defaultTimeout = 1801;

/* The first node in the global device list, or NULL if empty */
struct DeviceNode *g_deviceList = NULL;
ithread_mutex_t g_deviceListMutex;

char g_varCount[SERVICE_SERVCOUNT] ={ CONTROL_VARCOUNT };
int g_cpTimerLoopRun = 1;

/*  Device type for manageable device. */
const char g_deviceType[] = "urn:schemas-upnp-org:device:ManageableDevice:2";
const char g_friendlyName[] = "B2BUA";
/* Service types for config services. */
const char *g_serviceType[] = {"urn:schemas-upnp-org:service:ConfigurationManagement:2",};

static const char g_getValuesFormat[]= 
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\
<cms:ContentPathList xmlns=\"urn:schemas-upnp-org:dm:ConfigurationManagement\" \
xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"urn:schemas-upnp-org:dm:ConfigurationManagement http://www.upnp.org/schemas/dm/ConfigurationManagement-v2.xsd\">\
<ContentPath>%s</ContentPath></cms:ContentPathList>";

static const char g_setValuesFormat[]= 
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\
<cms:ParameterValueList xmlns=\"urn:schemas-upnp-org:dm:ConfigurationManagement\" \
xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"urn:schemas-upnp-org:dm:ConfigurationManagement http://www.upnp.org/schemas/dm/ConfigurationManagement-v2.xsd\">\
<Parameter><ParameterPath>%s</ParameterPath><Value>%s</Value></Parameter></cms:ParameterValueList>";

const char *g_serviceName[] = { "ConfigurationManagement", "" };
const char *g_varName[SERVICE_SERVCOUNT][CP_MAXVARS] = {
	{"ConfigurationUpdate","SupportedDataModelsUpdate","SupportedParametersUpdate","AttributeValuesUpdate","InconsistentStatus","AlarmsEnabled"}
};


/*! Tags for valid commands issued at the command prompt. */
enum cmdloop_cmds {
	Help = 0,
	ReFresh,
	ListDev,
	GetVar,
	SetAlarmsEnabled,
	GetValues,
	SetValues,
	ExitCmd
};

/*! Data structure for parsing commands from the command line. */
struct cmdloop_commands {
	const char *str;/* the string  */
	int command;/* the command */
	int numargs;/* the number of arguments */
	const char *args;/* the args */
} cmdloop_commands;

/*! Mappings between command text names, command tag,
* and required command arguments for command line
* commands */
static struct cmdloop_commands g_cmdList[] = {
	{"Help", Help,     1, ""},
	{"Refresh", ReFresh,     1, ""},
	{"List", ListDev,      2, "<devnum>"},
	{"GetVar", GetVar,  2, "<devnum> <varName (string)>"},
	{"SetAlarmsEnabled", SetAlarmsEnabled,  2, "<devnum> <0|1> "},
	{"GetValues", GetValues,  2, "<devnum> <nodePath (string)>"},
	{"SetValues", SetValues,  3, "<devnum> <nodePath (string)> <nodeValue (string)>"},
	{"Exit", ExitCmd, 1, ""}
};
void CtrlPointPrintHelp(void)
{
	printf("Commands:\n"
		"  Help\n"
		"  Refresh\n"
		"  List		[<devnum>]\n"
		"  GetVar	<devnum> <varname>\n"
		"  GetValues	<devnum> <nodePath>\n"
		"  SetAlarmsEnabled	 <devnum> <0|1>\n"
		"  SetValues	<devnum> <nodePath> <nodeValue>\n"
		"  Exit\n");
	printf("\n"
		"Detail:\n"
		"  Help\n"
		"       Print this help info.\n"
		"  Refresh\n"
		"       Delete all of the devices from the ManageableDevice list and issue new\n"
		"         search request to rebuild the list from scratch.\n"
		"  List  [<devnum>]\n"
		"       Print the state table for the ManageableDevice <devnum>.\n"
		"       IF no <devnum>,print the current list of ManageableDevice Emulators that this\n"
		"         control point is aware of. \n"
		"         e.g., List 1' prints the state table for the first device in the ManageableDevice list.\n"
		"  GetVar <devnum> <varname>\n"
		"       Requests the value of a variable specified by the string <varname>\n"
		"         from the Control Service of device <devnum>.\n"
		"         (e.g., \" GetVar  1  ConfigurationUpdate \")\n"
		"  GetValues <devnum> <nodepath> \n"
		"       Sends an action request specified by the string <GetValues>\n"
		"         to the Control Service of device <devnum>.\n"
		"         (e.g., \" GetValues  1  /BBF/VoiceService/0/SIP/Network/0/ProxyServer \")\n"
		"  SetAlarmsEnabled <devnum> <0|1>\n"
		"       1:will force the Parent Device from including the pair name-value for 'alarmed' parameters,if any in the ConfigurationUpdate state;\n"
		"       0:will prevent the Parent Device to include the pair name-value for 'alarmed' parameters,when they change their value.\n"
		"         (e.g., \" SetAlarmsEnabled  1  1 \")\n"
		"  SetValues <devnum> <nodepath> <nodevalue>\n"
		"       Sends an action request specified by the string <SetValues>\n"
		"         to the Control Service of device <devnum>.\n"
		"         (e.g., \" SetValues  1 /BBF/VoiceService/0/SIP/Network/0/ProxyServer 192.168.9.130 \")\n"
		"  Exit\n"
		"       Exits the control point application.\n");
}

int CtrlPointDeleteNode( struct DeviceNode *node )
{
	int rc, service, var;

	if (NULL == node) {
		printf("ERROR: CtrlPointDeleteNode: Node is empty\n");
		return -1;
	}
	for (service = 0; service < SERVICE_SERVCOUNT; service++) {
		/* If we have a valid control SID, then unsubscribe */
		if (strcmp(node->device.service[service].SID, "") != 0) {
			rc = UpnpUnSubscribe(g_cpHandle,node->device.service[service].SID);
			if (UPNP_E_SUCCESS == rc) {
				printf("Unsubscribed from %s eventURL with SID=%s\n",
					g_serviceName[service],node->device.service[service].SID);
			} else {
				printf("Error unsubscribing to %s eventURL -- %d\n",g_serviceName[service],rc);
			}
		}

		for (var = 0; var < g_varCount[service]; var++) {
			if (node->device.service[service].varStrVal[var]) {
				free(node->device.service[service].varStrVal[var]);
				node->device.service[service].varStrVal[var] = NULL;
			}
		}
	}

	/*Notify New Device Added */
	NotifyStateUpdate(NULL, NULL, node->device.UDN, DEVICE_REMOVED);

	free(node);
	node = NULL;
	return 0;
}

int CtrlPointRemoveDevice(const char *UDN)
{
	struct DeviceNode *curDevNode;
	struct DeviceNode *prevDevNode;

	ithread_mutex_lock(&g_deviceListMutex);
	curDevNode = g_deviceList;
	if (curDevNode) {
		if (0 == strcmp(curDevNode->device.UDN, UDN)) {
			g_deviceList = curDevNode->next;
			CtrlPointDeleteNode(curDevNode);
		} else {
			prevDevNode = curDevNode;
			curDevNode = curDevNode->next;
			while (curDevNode) {
				if (strcmp(curDevNode->device.UDN, UDN) == 0) {
					prevDevNode->next = curDevNode->next;
					CtrlPointDeleteNode(curDevNode);
					break;
				}
				prevDevNode = curDevNode;
				curDevNode = curDevNode->next;
			}
		}
	}
	ithread_mutex_unlock(&g_deviceListMutex);
	return 0;
}

int CtrlPointRemoveAll(void)
{
	struct DeviceNode *curDevNode, *next;

	ithread_mutex_lock(&g_deviceListMutex);
	curDevNode = g_deviceList;
	g_deviceList = NULL;
	while (curDevNode) {
		next = curDevNode->next;
		CtrlPointDeleteNode(curDevNode);
		curDevNode = next;
	}
	ithread_mutex_unlock(&g_deviceListMutex);
	return 0;
}

int CtrlPointRefresh(void)
{
	int rc;

	CtrlPointRemoveAll();

	/* Search for all devices of type ManageableDevice version 1,
	* waiting for up to 5 seconds for the response */
	rc = UpnpSearchAsync(g_cpHandle, 5, g_deviceType, NULL);
	if (UPNP_E_SUCCESS != rc) {
		printf("Error sending search request%d\n", rc);
		return rc;
	}
	return rc;
}

int CtrlPointGetVar(int service, int devnum, const char *varname)
{
	struct DeviceNode *devNode;
	int rc;

	ithread_mutex_lock(&g_deviceListMutex);
	rc = CtrlPointGetDevice(devnum, &devNode);
	if (0 == rc) {
		rc = UpnpGetServiceVarStatusAsync(
			g_cpHandle,devNode->device.service[service].controlURL,
			varname,CtrlPointCallbackEventHandler,NULL);
		if (rc != UPNP_E_SUCCESS) {
			printf("Error in UpnpGetServiceVarStatusAsync -- %d\n",rc);
			rc = -1;
		}
	}
	ithread_mutex_unlock(&g_deviceListMutex);
	return rc;
}

int CtrlPointGetDevice(int devnum, struct DeviceNode **devnode)
{
	int count = devnum;
	struct DeviceNode *tmpDevNode = NULL;

	if (count)
		tmpDevNode = g_deviceList;
	while (--count && tmpDevNode) {
		tmpDevNode = tmpDevNode->next;
	}
	if (!tmpDevNode) {
		printf("Error finding Device number -- %d\n",devnum);
		return -1;
	}
	*devnode = tmpDevNode;

	return 0;
}

int CtrlPointPrintList()
{
	struct DeviceNode *tmpDevNode;
	int i = 0;

	ithread_mutex_lock(&g_deviceListMutex);
	printf("CtrlPointPrintList:\n");
	tmpDevNode = g_deviceList;
	while (tmpDevNode) {
		printf(" %3d -- %s,%s\n", ++i, tmpDevNode->device.UDN,tmpDevNode->device.friendlyName);
		tmpDevNode = tmpDevNode->next;
	}
	printf("\n");
	ithread_mutex_unlock(&g_deviceListMutex);

	return 0;
}

int CtrlPointPrintDevice(int devnum)
{
	struct DeviceNode *tmpDevNode = NULL;
	int i = 0, service, var;
	char spacer[15]={0};

	if (devnum <= 0) {
		CtrlPointPrintList();
		return 0;
	}

	ithread_mutex_lock(&g_deviceListMutex);
	printf("PrintDevice:\n");
	tmpDevNode = g_deviceList;
	while (tmpDevNode) {
		i++;
		if (i == devnum) break;
		tmpDevNode = tmpDevNode->next;
	}
	if (!tmpDevNode) {
		printf("Error in PrintDevice: ""invalid devnum = %d  --  actual device count = %d\n",devnum, i);
	} else {
		printf("  Device -- %d\n"
			"    |                  \n"
			"    +- UDN        = %s\n"
			"    +- descDocURL     = %s\n"
			"    +- friendlyName   = %s\n"
			"    +- presURL        = %s\n"
			"    +- Adver. TimeOut = %d\n",
			devnum,
			tmpDevNode->device.UDN,
			tmpDevNode->device.descDocURL,
			tmpDevNode->device.friendlyName,
			tmpDevNode->device.presURL,
			tmpDevNode->device.advrTimeOut);
		for (service = 0; service < SERVICE_SERVCOUNT; service++) {
			if (service < SERVICE_SERVCOUNT-1) sprintf(spacer, "    |    ");
			else sprintf(spacer, "         ");
			printf("    |                  \n"
				"    +-  %s service\n"
				"%s+- serviceId       = %s\n"
				"%s+- serviceType     = %s\n"
				"%s+- eventURL        = %s\n"
				"%s+- controlURL      = %s\n"
				"%s+- SID             = %s\n"
				"%s+- ServiceStateTable\n",
				g_serviceName[service],
				spacer,
				tmpDevNode->device.service[service].serviceId,
				spacer,
				tmpDevNode->device.service[service].serviceType,
				spacer,
				tmpDevNode->device.service[service].eventURL,
				spacer,
				tmpDevNode->device.service[service].controlURL,
				spacer,
				tmpDevNode->device.service[service].SID,
				spacer);
			for (var = 0; var < g_varCount[service]; var++) {
				printf("%s     +- %-10s = %s\n",spacer,g_varName[service][var],tmpDevNode->device.service[service].varStrVal[var]);
			}
		}
	}
	printf("\n");
	ithread_mutex_unlock(&g_deviceListMutex);
	return 0;
}

void CtrlPointAddDevice(IXML_Document *doc,const char *location,int expires)
{
	char *deviceType = NULL;
	char *friendlyName = NULL;
	char presURL[NAME_SIZE]={0};
	char *baseURL = NULL;
	char *relURL = NULL;
	char *UDN = NULL;
	char uuid[NAME_SIZE]={0};
	char *serviceId[SERVICE_SERVCOUNT] = { NULL };
	char *eventURL[SERVICE_SERVCOUNT] = { NULL };
	char *controlURL[SERVICE_SERVCOUNT] = { NULL };
	Upnp_SID eventSID[SERVICE_SERVCOUNT]={0};
	int timeOut[SERVICE_SERVCOUNT] = {g_defaultTimeout};
	struct DeviceNode *deviceNode = NULL;
	struct DeviceNode *tmpDevNode = NULL;
	int ret = 1;
	int found = 0;
	int service;
	int var;

	ithread_mutex_lock(&g_deviceListMutex);

	/* Read key elements from description document */
	UDN = GetFirstDocumentItem(doc, "UDN");
	deviceType = GetFirstDocumentItem(doc, "deviceType");
	friendlyName = GetFirstDocumentItem(doc, "friendlyName");
	baseURL = GetFirstDocumentItem(doc, "URLBase");
	relURL = GetFirstDocumentItem(doc, "presentationURL");

	ret = UpnpResolveURL((baseURL ? baseURL : location), relURL, presURL);
	if (NULL != deviceType 
		&& 0 == strncasecmp(deviceType, g_deviceType,strlen(g_deviceType))
		&& 0 == strncasecmp(friendlyName, g_friendlyName,strlen(g_friendlyName))) {

			/* Check if this device is already in the list */
			tmpDevNode = g_deviceList;
			while (tmpDevNode) {
				if ( NULL != UDN && strcmp(tmpDevNode->device.UDN, UDN) == 0) {
					found = 1;
					break;
				}
				tmpDevNode = tmpDevNode->next;
			}

			if (found) {
				/* The device is already there, so just update  */
				/* the advertisement timeout field */
				tmpDevNode->device.advrTimeOut = expires;
			} else {
				for (service = 0; service < SERVICE_SERVCOUNT;service++) {
					if (FindAndParseService(doc, location, g_serviceType[service],
						&serviceId[service], &eventURL[service],&controlURL[service])) {
							printf("Subscribing to eventURL %s...\n", eventURL[service]);
							ret = UpnpSubscribe(g_cpHandle,eventURL[service],&timeOut[service],eventSID[service]);
							if (ret == UPNP_E_SUCCESS) {
								printf("Subscribed to eventURL with SID=%s\n",eventSID[service]);
							} else {
								printf("Error Subscribing to eventURL -- %d\n",ret);
								strcpy(eventSID[service], "");
							}
					} 
				}
				/* Create a new device node */
				deviceNode =(struct DeviceNode *)malloc(sizeof(struct DeviceNode));
				memset(deviceNode,0, sizeof(deviceNode));
				memset(deviceNode->device.UDN,0,sizeof(deviceNode->device.UDN));
				if(UDN)
					strncpy(deviceNode->device.UDN, UDN, sizeof(deviceNode->device.UDN)-1);
				memset(deviceNode->device.descDocURL,0,sizeof(deviceNode->device.descDocURL));
				if(location)
					strncpy(deviceNode->device.descDocURL,location,sizeof(deviceNode->device.descDocURL)-1);
				memset(deviceNode->device.friendlyName,0,sizeof(deviceNode->device.friendlyName));
				if(friendlyName)
					strncpy(deviceNode->device.friendlyName,friendlyName,sizeof(deviceNode->device.friendlyName)-1);
				memset(deviceNode->device.presURL,0,sizeof(deviceNode->device.presURL));
				strncpy(deviceNode->device.presURL,presURL,sizeof(deviceNode->device.presURL)-1);
				deviceNode->device.advrTimeOut = expires;
				for (service = 0; service < SERVICE_SERVCOUNT;service++) {
					memset(deviceNode->device.service[service].serviceId,0,sizeof(deviceNode->device.service[service].serviceId));
					if(NULL != serviceId[service]) 
						strcpy(deviceNode->device.service[service].serviceId, serviceId[service]);
					memset(deviceNode->device.service[service].serviceType ,0, sizeof(deviceNode->device.service[service].serviceType));
					if(NULL != g_serviceType[service])
						strcpy(deviceNode->device.service[service].serviceType, g_serviceType[service]);
					memset( deviceNode->device.service[service].controlURL,0,sizeof(deviceNode->device.service[service].controlURL) );
					if(NULL != controlURL[service])
						strcpy(deviceNode->device.service[service].controlURL, controlURL[service]);
					memset(deviceNode->device.service[service].eventURL ,0,sizeof(deviceNode->device.service[service].eventURL) );
					if(NULL != eventURL[service])
						strcpy(deviceNode->device.service[service].eventURL, eventURL[service]);
					memset( deviceNode->device.service[service].SID,0,sizeof(deviceNode->device.service[service].SID) );
					if(NULL != eventSID[service])
						strcpy(deviceNode->device.service[service].SID, eventSID[service]);
					for (var = 0; var < g_varCount[service]; var++) {
						deviceNode->device.service[service].varStrVal[var] =	(char *)malloc(MAX_VAL_LEN);
						memset(deviceNode->device.service[service].varStrVal[var],0,MAX_VAL_LEN);
						strcpy(deviceNode->device.service[service].varStrVal[var], "");
					}
				}
				deviceNode->next = NULL;
				/* Insert the new device node in the list */
				if ((tmpDevNode = g_deviceList)) {
					while (tmpDevNode) {
						if (tmpDevNode->next) {
							tmpDevNode = tmpDevNode->next;
						} else {
							tmpDevNode->next = deviceNode;
							break;
						}
					}
				} else {
					g_deviceList = deviceNode;
				}
				/*Notify New Device Added */
				NotifyStateUpdate(NULL, NULL,deviceNode->device.UDN,DEVICE_ADDED);
			}
	}

	ithread_mutex_unlock(&g_deviceListMutex);

	if (deviceType) free(deviceType);
	if (friendlyName) free(friendlyName);
	if (UDN) free(UDN);
	if (baseURL) free(baseURL);
	if (relURL) free(relURL);
	for (service = 0; service < SERVICE_SERVCOUNT; service++) {
		if (serviceId[service]) free(serviceId[service]);
		if (controlURL[service]) free(controlURL[service]);
		if (eventURL[service]) free(eventURL[service]);
	}
}

char *str_sub(const char *st, const char *orig, char *repl) 
{
	char *buffer = strdup("");
	const char *p = st;
	const char *ch = strstr(p, orig);

	while (ch != NULL) {
		buffer = (char *)realloc(buffer, (strlen(buffer) + (ch-p ) + strlen(repl) + 1) * sizeof(char));
		buffer = strncat(buffer, p, ch-p);
		buffer = strcat(buffer, repl);
		p = ch + strlen(orig);
		ch = strstr(p, orig);
	}
	buffer = (char *)realloc(buffer, (strlen(buffer) + strlen(p) + 1) * sizeof(char));
	buffer = strcat(buffer, p);
	return buffer;
}

char *Unescaped(const char *st)
{
	char *st_in = strdup(st);
	char *st_out;
	int i;

	struct {
		char *in;
		char *out;
	} pattern[] = {
		{ "&amp;",  "&"  },
		{ "&apos;", "'"  },
		{ "&lt;",   "<"  },
		{ "&gt;",   ">"  },
		{ "&quot;", "\"" }
	};

	for (i=0; i<sizeof(pattern)/sizeof(pattern[0]); i++) {
		st_out = str_sub(st_in, pattern[i].in, pattern[i].out );
		free(st_in);
		st_in = st_out;
	}
	return st_in;
}

char *Escaped(const char *st) 
{
	char *st_in = strdup(st);
	char *st_out;
	int i;

	struct {
		char *in;
		char *out;
	} pattern[] = {
		{ "&amp;",  "&"  },
		{ "&apos;", "'"  },
		{ "&lt;",   "<"  },
		{ "&gt;",   ">"  },
		{ "&quot;", "\"" }
	};

	for (i=0; i<sizeof(pattern)/sizeof(pattern[0]); i++) {
		st_out = str_sub(st_in, pattern[i].out, pattern[i].in );
		free(st_in);
		st_in = st_out;
	}
	return st_in;
}


/* 
状态变量g_varName变化通知，含被修改节点变化通知(AlarmsEnabled=1)，如:
ConfigurationUpdate='24,2015-07-27T20:47:22,<?xml version="1.0" encoding="UTF-8"?>
<cms:ParameterValueList xmlns:cms="urn:schemas-upnp-org:dm:cms" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="urn:schemas-upnp-org:dm:cms http://www.upnp.org/schemas/dm/cms.xsd">
	<Parameter>
		<ParameterPath>/BBF/VoiceService/0/SIP/Network/0/Status</ParameterPath>
		<Value>Up</Value>
	</Parameter>
</cms:ParameterValueList>'
*/
void StateVarUpdate(char *UDN, int service, IXML_Document *changedVariables,char **state)
{
	IXML_NodeList *properties;
	IXML_NodeList *variables;
	IXML_Element *property;
	IXML_Element *variable;
	unsigned int length;
	unsigned int length1;
	unsigned int i;
	int j;

	printf("StateUpdate (service %d):\n", service);

	/* Find all of the e:property tags in the document */
	properties = ixmlDocument_getElementsByTagName(changedVariables,"e:property");
	if (properties) 
	{
		length = ixmlNodeList_length(properties);
		for (i = 0; i < length; i++) 
		{
			/* Loop through each property change found */
			property = (IXML_Element *)ixmlNodeList_item(properties, i);
			/* For each variable name in the state table,check if this is a corresponding property change */
			for (j = 0; j < g_varCount[service]; j++) 
			{
				variables = ixmlElement_getElementsByTagName(property,g_varName[service][j]);
				/* If a match is found, extract the value, and update the state table */
				if (variables) 
				{
					length1 = ixmlNodeList_length(variables);
					if (length1) 
					{
						char *tmpState = NULL;
						variable = (IXML_Element *)ixmlNodeList_item(variables, 0);
						tmpState = GetElementValue(variable);
						if (tmpState) 
						{
							int ret = 0;
							char version[NAME_SIZE]={0};
							char lastDateTime[NAME_SIZE]={0};
							char xmlBuffer[MAX_BUFFER]={0};
							strncpy(state[j], tmpState,MAX_VAL_LEN-1);
							printf(" %s='%s'\n", g_varName[service][j],tmpState);
							ret = sscanf(tmpState,"%[^,],%[^,],%[^,]",version,lastDateTime,xmlBuffer);
							if (3 == ret ) 
							{
								char *unescaped = NULL;
								unescaped = Unescaped(xmlBuffer);
								if( NULL != unescaped)
								{
									PrintParameters(unescaped);
									free(unescaped);
								}
							}
							free(tmpState);
						}
					}
					ixmlNodeList_free(variables);
					variables = NULL;
				}
			}
		}
		ixmlNodeList_free(properties);
	}
	return;
}

void CtrlPointHandleEvent(const char *sid,int evntkey,IXML_Document *changes)
{
	struct DeviceNode *tmpDevNode;
	int service;

	ithread_mutex_lock(&g_deviceListMutex);
	tmpDevNode = g_deviceList;
	while (tmpDevNode) {
		for (service = 0; service < SERVICE_SERVCOUNT; ++service) {
			if (strcmp(tmpDevNode->device.service[service].SID, sid)== 0) {
				printf("Received %s Event: %d for SID %s\n",g_serviceName[service],evntkey,sid);
				StateVarUpdate(tmpDevNode->device.UDN,service,changes,
					(char **)&tmpDevNode->device.service[service].varStrVal);
				break;
			}
		}
		tmpDevNode = tmpDevNode->next;
	}
	ithread_mutex_unlock(&g_deviceListMutex);
}

void CtrlPointHandleSubscribeUpdate(const char *eventURL,const Upnp_SID sid,int timeout)
{
	struct DeviceNode *tmpDevNode;
	int service;

	ithread_mutex_lock(&g_deviceListMutex);
	tmpDevNode = g_deviceList;
	while (tmpDevNode) {
		for (service = 0; service < SERVICE_SERVCOUNT; service++){
			if (strcmp(tmpDevNode->device.service[service].eventURL,eventURL) == 0) {
				printf("Received %s Event Renewal for eventURL %s\n",
					g_serviceName[service], eventURL);
				strcpy(tmpDevNode->device.service[service].SID, sid);
				break;
			}
		}
		tmpDevNode = tmpDevNode->next;
	}
	ithread_mutex_unlock(&g_deviceListMutex);

	return;
	timeout = timeout;
}

void CtrlPointHandleGetVar(const char *controlURL,const char *varName,const DOMString varValue)
{
	struct DeviceNode *tmpDevNode;
	int service;

	ithread_mutex_lock(&g_deviceListMutex);
	tmpDevNode = g_deviceList;
	while (tmpDevNode) {
		for (service = 0; service < SERVICE_SERVCOUNT; service++) {
			if (strcmp(tmpDevNode->device.service[service].controlURL,controlURL) == 0) {
				NotifyStateUpdate(varName,varValue,tmpDevNode->device.UDN,GET_VAR_COMPLETE);
				break;
			}
		}
		tmpDevNode = tmpDevNode->next;
	}
	ithread_mutex_unlock(&g_deviceListMutex);
}

void CtrlPointVerifyTimeouts(int incr)
{
	int ret;
	struct DeviceNode *prevDevNode = NULL;
	struct DeviceNode *curDevNode = NULL;

	ithread_mutex_lock(&g_deviceListMutex);
	curDevNode = g_deviceList;
	while (curDevNode) {
		curDevNode->device.advrTimeOut -= incr;
		/*printf("Advertisement Timeout: %d\n", curDevNode->device.advrTimeOut); */
		if (curDevNode->device.advrTimeOut <= 0) {
			/* This advertisement has expired, so we should remove the device from the list */
			if (g_deviceList == curDevNode) g_deviceList = curDevNode->next;
			else prevDevNode->next = curDevNode->next;
			CtrlPointDeleteNode(curDevNode);
			if (prevDevNode) curDevNode = prevDevNode->next;
			else curDevNode = g_deviceList;
		} else {
			if (curDevNode->device.advrTimeOut < 2 * incr) {
				/* This advertisement is about to expire, so
				* send out a search request for this device UDN to try to renew */
				ret = UpnpSearchAsync(g_cpHandle, incr,curDevNode->device.UDN,NULL);
				printf("sending search request for Device UDN: %s -- ret = %d\n",curDevNode->device.UDN, ret);
				if (ret != UPNP_E_SUCCESS)
					printf("Error sending search request for Device UDN: %s -- err = %d\n",
					curDevNode->device.UDN, ret);
			}
			prevDevNode = curDevNode;
			curDevNode = curDevNode->next;
		}
	}
	ithread_mutex_unlock(&g_deviceListMutex);
}

void *CtrlPointTimerLoop(void *args)
{
	/* how often to verify the timeouts, in seconds */
	int incr = 30;

	while (g_cpTimerLoopRun) {
		isleep((unsigned int)incr);
		CtrlPointVerifyTimeouts(incr);
	}
	ithread_detach(ithread_self());
	return NULL;
}

int CtrlPointStart()
{
	ithread_t timerThread;
	int rc;
	unsigned short port = 0;
	char *ipAddress = NULL;

	ithread_mutex_init(&g_deviceListMutex, 0);
	printf("CtrlPointStart with paddress=%s port=%u\n",ipAddress ? ipAddress :"{NULL}",port);
	rc = UpnpInit(ipAddress, port);
	if (rc != UPNP_E_SUCCESS) {
		printf("WinCEStart: UpnpInit() Error: %d\n", rc);
		UpnpFinish();
		return -1;
	}
	if (!ipAddress)  ipAddress = UpnpGetServerIpAddress();
	if (!port)  port = UpnpGetServerPort();

	printf("UPnP CP Initialized ipaddress=%s port=%u\n",ipAddress ? ipAddress:"{NULL}",port);
	rc = UpnpRegisterClient(CtrlPointCallbackEventHandler,&g_cpHandle,&g_cpHandle);
	if (rc != UPNP_E_SUCCESS) {
		printf("Error registering CP: %d\n", rc);
		UpnpFinish();
		return -1;
	}
	printf("Config Control Point Registered\n");

	/* start a timer thread */
	ithread_create(&timerThread, NULL, CtrlPointTimerLoop, NULL);
	
	return 0;
}

int CtrlPointStop(void)
{
	CtrlPointRemoveAll();
	UpnpUnRegisterClient(g_cpHandle );
	UpnpFinish();
	ithread_mutex_destroy(&g_deviceListMutex);
	return 0;
}

void *CtrlPointCommandLoop(void *args)
{
	CtrlPointRefresh();

	while (1) {
		char cmdline[MAX_BUFFER]={0};
		printf("\n>");
		fgets(cmdline, sizeof(cmdline)-1, stdin);
		CtrlPointProcessCommand(cmdline);
	}
	ithread_detach(ithread_self());
	return NULL;
}

int GetValueSendAction(ActionParam* action)
{
	int rc = 0;
	int i;
	struct DeviceNode *devNode;
	IXML_Document *actionNode = NULL;
	int service;
	char *actionName = NULL;
	char nodeBuffer[MAX_BUFFER]={0};
	int numOfCmds = (sizeof g_cmdList)/sizeof (cmdloop_commands);

	ithread_mutex_lock(&g_deviceListMutex);
	rc = CtrlPointGetDevice(action->devnum, &devNode);
	if (rc<0) {
		printf("Can't find device %d\n",action->devnum);
		ithread_mutex_unlock(&g_deviceListMutex);
		return -1;
	}

	service = action->serviceType;
	for (i = 0; i < numOfCmds; ++i) {
		if ( action->actionType == g_cmdList[i].command) {
			actionName=(char *)g_cmdList[i].str;
			break;
		}
	}

	actionNode=UpnpMakeAction(actionName, g_serviceType[service],0, NULL);
	if(actionNode==NULL){
		printf("UpnpMakeAction failed\n");
		ithread_mutex_unlock(&g_deviceListMutex);
		return -1;
	}

	snprintf(nodeBuffer, sizeof(nodeBuffer)-1,g_getValuesFormat, action->paramName);
	rc=UpnpAddToAction(&actionNode,actionName,g_serviceType[service],"Parameters",nodeBuffer);

	rc = UpnpSendActionAsync(g_cpHandle,devNode->device.service[service].controlURL,
		g_serviceType[service], NULL,actionNode,CtrlPointCallbackEventHandler, NULL);
	if(rc!=UPNP_E_SUCCESS) printf("Error in UpnpSendActionAsync -- %d\n",rc);
	ithread_mutex_unlock(&g_deviceListMutex);
	
	if (actionNode){
		ixmlDocument_free(actionNode);
	}
	return 0;
}

int SetValueSendAction(ActionParam* action)
{
	int rc = 0;
	int i;
	struct DeviceNode *devNode;
	IXML_Document *actionNode = NULL;
	int service;
	char *actionName = NULL;
	char nodeBuffer[MAX_BUFFER]={0};
	int numOfCmds = (sizeof g_cmdList) / sizeof (cmdloop_commands);

	ithread_mutex_lock(&g_deviceListMutex);
	rc = CtrlPointGetDevice(action->devnum, &devNode);
	if (rc<0) {
		printf("Can't find device %d\n",action->devnum);
		ithread_mutex_unlock(&g_deviceListMutex);
		return -1;
	}

	service = action->serviceType;
	for (i = 0; i < numOfCmds; ++i) {
		if ( action->actionType == g_cmdList[i].command) {
			actionName=(char *)g_cmdList[i].str;
			break;
		}
	}

	actionNode=UpnpMakeAction(actionName, g_serviceType[service],0, NULL);
	if(actionNode==NULL){
		printf("UpnpMakeAction failed\n");
		ithread_mutex_unlock(&g_deviceListMutex);
		return -1;
	}

	snprintf(nodeBuffer, sizeof(nodeBuffer)-1, g_setValuesFormat, action->paramName,action->paramValue);
	rc=UpnpAddToAction(&actionNode,actionName,g_serviceType[service],"ParameterValueList",nodeBuffer);

	rc = UpnpSendActionAsync(g_cpHandle,devNode->device.service[service].controlURL,
		g_serviceType[service], NULL,actionNode,CtrlPointCallbackEventHandler, NULL);
	if(rc!=UPNP_E_SUCCESS) printf("Error in UpnpSendActionAsync -- %d\n",rc);
	ithread_mutex_unlock(&g_deviceListMutex);

	if (actionNode){
		ixmlDocument_free(actionNode);
	}
	return 0;
}

int SetAlarmsEnabledSendAction(ActionParam* action)
{
	int rc = 0;
	int i;
	struct DeviceNode *devNode;
	IXML_Document *actionNode = NULL;
	int service;
	char *actionName = NULL;
	int numOfCmds = (sizeof g_cmdList) /sizeof (cmdloop_commands);

	ithread_mutex_lock(&g_deviceListMutex);
	rc = CtrlPointGetDevice(action->devnum, &devNode);
	if (rc<0) {
		printf("Can't find device %d\n",action->devnum);
		ithread_mutex_unlock(&g_deviceListMutex);
		return -1;
	}

	service = action->serviceType;
	for (i = 0; i < numOfCmds; ++i) {
		if ( action->actionType == g_cmdList[i].command) {
			actionName=(char *)g_cmdList[i].str;
			break;
		}
	}

	actionNode=UpnpMakeAction(actionName, g_serviceType[service],0, NULL);
	if(actionNode==NULL){
		printf("UpnpMakeAction failed\n");
		ithread_mutex_unlock(&g_deviceListMutex);
		return -1;
	}

	rc=UpnpAddToAction(&actionNode,actionName,g_serviceType[service],
		action->paramName,action->paramValue);

	rc = UpnpSendActionAsync(g_cpHandle,devNode->device.service[service].controlURL,
		g_serviceType[service], NULL,actionNode,CtrlPointCallbackEventHandler, NULL);
	if(rc!=UPNP_E_SUCCESS) printf("Error in UpnpSendActionAsync -- %d\n",rc);
	ithread_mutex_unlock(&g_deviceListMutex);

	if (actionNode){
		ixmlDocument_free(actionNode);
	}
	return 0;
}


char *GetElementValue(IXML_Element *element)
{
	IXML_Node *child = ixmlNode_getFirstChild((IXML_Node *)element);
	char *temp = NULL;

	if (child != 0 && ixmlNode_getNodeType(child) == eTEXT_NODE)
		temp = strdup(ixmlNode_getNodeValue(child));

	return temp;
}

IXML_NodeList *GetFirstServiceList(IXML_Document *doc)
{
	IXML_NodeList *ServiceList = NULL;
	IXML_NodeList *servlistnodelist = NULL;
	IXML_Node *servlistnode = NULL;

	servlistnodelist =
		ixmlDocument_getElementsByTagName(doc, "serviceList");
	if (servlistnodelist && ixmlNodeList_length(servlistnodelist)) {
		/* we only care about the first service list, from the root device */
		servlistnode = ixmlNodeList_item(servlistnodelist, 0);
		/* create as list of DOM nodes */
		ServiceList = ixmlElement_getElementsByTagName(
			(IXML_Element *)servlistnode, "service");
	}
	if (servlistnodelist)
		ixmlNodeList_free(servlistnodelist);

	return ServiceList;
}


char *GetFirstDocumentItem(IXML_Document *doc, const char *item)
{
	IXML_NodeList *nodeList = NULL;
	IXML_Node *textNode = NULL;
	IXML_Node *tmpNode = NULL;
	char *ret = NULL;

	nodeList = ixmlDocument_getElementsByTagName(doc, (char *)item);
	if (nodeList) {
		tmpNode = ixmlNodeList_item(nodeList, 0);
		if (tmpNode) {
			textNode = ixmlNode_getFirstChild(tmpNode);
			if (!textNode) {
				ret = strdup("");
				goto epilogue;
			}
			ret = strdup(ixmlNode_getNodeValue(textNode));
			if (!ret) {
				printf("ixmlNode_getNodeValue returned NULL\n"); 
				ret = strdup("");
			}
		}
	} 
epilogue:
	if (nodeList)
		ixmlNodeList_free(nodeList);
	return ret;
}

char *GetFirstElementItem(IXML_Element *element, const char *item)
{
	IXML_NodeList *nodeList = NULL;
	IXML_Node *textNode = NULL;
	IXML_Node *tmpNode = NULL;
	char *ret = NULL;

	nodeList = ixmlElement_getElementsByTagName(element, (char *)item);
	if (nodeList == NULL) {
		printf("Error finding %s in XML Node\n",item);
		return NULL;
	}
	tmpNode = ixmlNodeList_item(nodeList, 0);
	if (!tmpNode) {
		printf("Error finding %s value in XML Node\n",item);
		ixmlNodeList_free(nodeList);
		return NULL;
	}
	textNode = ixmlNode_getFirstChild(tmpNode);
	ret = strdup(ixmlNode_getNodeValue(textNode));
	if (!ret) {
		printf("Error allocating memory for %s in XML Node\n",item);
		ixmlNodeList_free(nodeList);
		return NULL;
	}
	ixmlNodeList_free(nodeList);

	return ret;
}

int FindAndParseService(IXML_Document *doc, const char *location,
	const char *serviceType, char **serviceId, char **eventURL, char **controlURL)
{
	unsigned int i;
	unsigned long length;
	int found = 0;
	int ret;
	unsigned int sindex = 0;
	char *tempServiceType = NULL;
	char *baseURL = NULL;
	const char *base = NULL;
	char *relcontrolURL = NULL;
	char *releventURL = NULL;
	IXML_NodeList *serviceList = NULL;
	IXML_Element *service = NULL;

	baseURL = GetFirstDocumentItem(doc,"URLBase");
	if (baseURL) base = baseURL;
	else base = location;

	serviceList = GetFirstServiceList(doc);
	length = ixmlNodeList_length(serviceList);
	for (i = 0; i < length; i++) {
		service = (IXML_Element *)ixmlNodeList_item(serviceList, i);
		if( tempServiceType) free(tempServiceType);
		tempServiceType = GetFirstElementItem((IXML_Element *)service,"serviceType");
		if (tempServiceType && serviceType && strcmp(tempServiceType, serviceType) == 0) 
		{
			printf("Found service: %s\n", serviceType);
			*serviceId = GetFirstElementItem(service, "serviceId");
			printf("serviceId: %s\n", *serviceId);
			relcontrolURL = GetFirstElementItem(service, "controlURL");
			releventURL = GetFirstElementItem(service, "eventSubURL");
			*controlURL = malloc(strlen(base) + strlen(relcontrolURL) + 1);
			if (*controlURL) {
				ret = UpnpResolveURL(base, relcontrolURL, *controlURL);
				if (ret != UPNP_E_SUCCESS)
					printf("Error generating controlURL from %s + %s\n",base,relcontrolURL);
			}
			*eventURL = malloc(strlen(base) + strlen(releventURL) + 1);
			if (*eventURL) {
				ret = UpnpResolveURL(base, releventURL, *eventURL);
				if (ret != UPNP_E_SUCCESS)
					printf("Error generating eventURL from %s + %s\n",base,releventURL);
			}
			free(relcontrolURL);
			free(releventURL);
			relcontrolURL = NULL;
			releventURL = NULL;
			found = 1;
			break;
		}
	}
	if( tempServiceType)	free(tempServiceType);
	if(serviceList) 	ixmlNodeList_free(serviceList);
	if(baseURL) free(baseURL);
	return found;
}

void NotifyStateUpdate(const char *varName,const char *varValue,const char *UDN,eventType type)
{
	printf("NotifyState %s=%s,UDN=%s,type=%d\n",varName,varValue,UDN,type);
}

void PrintParameters(const char *buffer)
{
	IXML_Document *doc = NULL;

	//Parses an XML text buffer converting it into an IXML DOM 
	doc = ixmlParseBuffer(buffer); 
	if(NULL != doc){
		IXML_NodeList *params = NULL;
		params = ixmlDocument_getElementsByTagName(doc,"Parameter");
		if (params) { 
			unsigned int numOfElem = 0;
			unsigned int i = 0;
			numOfElem = ixmlNodeList_length(params);
			for (i = 0; i < numOfElem; i++) { //Parameter
				IXML_Element *element = NULL;
				element = (IXML_Element *)ixmlNodeList_item(params, i);
				if( NULL != element){
					IXML_NodeList *pathNodeList = NULL;
					pathNodeList = ixmlElement_getElementsByTagName((IXML_Element *)element,"ParameterPath");
					if (NULL !=pathNodeList) { // ParameterPath & Value
						char *pathNodeValue = NULL;
						IXML_NodeList *valueNodeList = NULL;
						IXML_Element *pathElement = NULL;
						pathElement = (IXML_Element *)ixmlNodeList_item(pathNodeList, 0);
						pathNodeValue = GetElementValue(pathElement);
						valueNodeList = ixmlElement_getElementsByTagName((IXML_Element *)element,"Value");
						if (NULL != valueNodeList) {
							IXML_Element *valueElement = NULL;
							char *valueNodeValue = NULL;
							
							valueElement = (IXML_Element *)ixmlNodeList_item(valueNodeList, 0);
							valueNodeValue = GetElementValue(valueElement);
							if( valueNodeValue) {
								printf("\n%s=%s\n",pathNodeValue,valueNodeValue);
								free(valueNodeValue);
							}
							ixmlNodeList_free(valueNodeList);
						}
						if( pathNodeValue) free(pathNodeValue);
						ixmlNodeList_free(pathNodeList);
					}
				}
			}
			ixmlNodeList_free(params);
			params = NULL;
		}
		ixmlDocument_free(doc);
		doc = NULL;
	}
	return;
}

int CtrlPointCallbackEventHandler(Upnp_EventType eventType, void *event, void *cookie)
{
	int ret;
	struct Upnp_Discovery *dEvent = NULL;
	struct Upnp_Event *eEvent = NULL;
	struct Upnp_Action_Complete *aEvent = NULL;
	struct Upnp_State_Var_Complete *svEvent = NULL;
	struct Upnp_Event_Subscribe *esEvent = NULL;
	IXML_Document *doc = NULL;
	int timeOut = g_defaultTimeout;
	Upnp_SID newSID;

	switch(eventType ) {
		/* SSDP Stuff */
		case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		case UPNP_DISCOVERY_SEARCH_RESULT: 
			dEvent = (struct Upnp_Discovery *)event;
			ret = UpnpDownloadXmlDoc(dEvent->Location, &doc);
			if (ret == UPNP_E_SUCCESS){
				CtrlPointAddDevice(doc,dEvent->Location,dEvent->Expires);
			}
			if (doc) ixmlDocument_free(doc);
			break;
		case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE: 
			dEvent = (struct Upnp_Discovery *)event;
			printf("Received ByeBye for Device: %s\n",dEvent->DeviceId);
			CtrlPointRemoveDevice(dEvent->DeviceId);
			break;
		case UPNP_DISCOVERY_SEARCH_TIMEOUT:
			break;
		/* SOAP Stuff */
		case UPNP_CONTROL_ACTION_COMPLETE:
			aEvent = (struct Upnp_Action_Complete *)event;
			printf("ErrCode = %s(%d)\n",UpnpGetErrorMessage(aEvent->ErrCode),aEvent->ErrCode);
			if (aEvent->ActionResult) {
				char* ParameterValueList = NULL;
				ParameterValueList = GetFirstDocumentItem(aEvent->ActionResult,"ParameterValueList");
				if( NULL != ParameterValueList) { //GetValues
					PrintParameters(ParameterValueList); 
					free(ParameterValueList);
				}
			} 
			break;
		case UPNP_CONTROL_GET_VAR_COMPLETE: 
			svEvent = (struct Upnp_State_Var_Complete *)event;
			if (svEvent->ErrCode == UPNP_E_SUCCESS) {
				CtrlPointHandleGetVar(svEvent->CtrlUrl,svEvent->StateVarName,svEvent->CurrentVal);
			}
			break;
		case UPNP_CONTROL_GET_VAR_REQUEST:
		case UPNP_CONTROL_ACTION_REQUEST:
			break;
			/* GENA Stuff */
		case UPNP_EVENT_RECEIVED: 
			eEvent = (struct Upnp_Event *)event;
			CtrlPointHandleEvent(eEvent->Sid,eEvent->EventKey,eEvent->ChangedVariables);
			break;
		case UPNP_EVENT_SUBSCRIBE_COMPLETE:
		case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
		case UPNP_EVENT_RENEWAL_COMPLETE: 
			esEvent = (struct Upnp_Event_Subscribe *)event;
			if (esEvent->ErrCode == UPNP_E_SUCCESS) {
				CtrlPointHandleSubscribeUpdate(esEvent->PublisherUrl,esEvent->Sid,esEvent->TimeOut);
			}
			break;
		case UPNP_EVENT_AUTORENEWAL_FAILED:
		case UPNP_EVENT_SUBSCRIPTION_EXPIRED: 
			esEvent = (struct Upnp_Event_Subscribe *)event;
			ret = UpnpSubscribe(g_cpHandle,esEvent->PublisherUrl,&timeOut,newSID);
			if (ret == UPNP_E_SUCCESS) {
				printf("Subscribed to eventURL with SID=%s\n", newSID);
				CtrlPointHandleSubscribeUpdate(esEvent->PublisherUrl,newSID,timeOut);
			} 
			break;
		case UPNP_EVENT_SUBSCRIPTION_REQUEST:
			break;
		default:
			break;
	}
	return 0;
}

int CtrlPointProcessCommand(char *cmdline)
{
	char cmd[MAX_BUFFER]={0};
	char strarg[NAME_SIZE]={0};
	int arg1 = -1;
	int command = -1;
	int numOfCmds = (sizeof g_cmdList) /sizeof (cmdloop_commands);
	int i;
	int rc;
	int validargs;
	int ret = -1;
	ActionParam action;

	memset(&action,0,sizeof(action));
	validargs = sscanf(cmdline, "%s %d", cmd, &arg1);
	for (i = 0; i < numOfCmds; ++i) {
		if(!strncasecmp(cmd,g_cmdList[i].str,strlen(g_cmdList[i].str))) {
			command = g_cmdList[i].command;
			break;
		}
	}
	switch (command) {
		case Help:
			CtrlPointPrintHelp();
			break;
		case GetVar:
			validargs = sscanf(cmdline, "%s %d %s", cmd, &arg1, strarg);
			if (validargs >= 3) CtrlPointGetVar(SERVICE_CONTROL, arg1, strarg);
			break;
		case ListDev:
			validargs = sscanf(cmdline, "%s %d", cmd, &arg1);
			CtrlPointPrintDevice(arg1);
			break;
		case ReFresh:
			CtrlPointRefresh();
			break;
		case ExitCmd:
			rc = CtrlPointStop();
			exit(rc);
			break;
		case SetAlarmsEnabled:
			{
				char value[NAME_SIZE]={0};
				validargs = sscanf(cmdline, "%s %d %s", cmd, &arg1, value);
				action.devnum = arg1;
				action.serviceType=SERVICE_CONTROL;
				action.actionType = SetAlarmsEnabled;
				strncpy(action.paramName, "StateVariableValue", sizeof(action.paramName)-1 );
				strncpy(action.paramValue, value, sizeof(action.paramValue)-1 );
				ret=SetAlarmsEnabledSendAction(&action);
				if(ret<0)	printf("SetAlarmsEnabledSendAction failed %d\n",ret);
			}
			break;
		case GetValues:
			{
				char path[NAME_SIZE]={0};
				validargs = sscanf(cmdline, "%s %d %s", cmd, &arg1, path);
				action.devnum = arg1;
				action.serviceType=SERVICE_CONTROL;
				action.actionType = GetValues;
				strncpy(action.paramName, path, sizeof(action.paramName)-1 );
				ret=GetValueSendAction(&action);
				if(ret<0)	printf("GetValueSendAction failed %d\n",ret);
			}
			break;	
		case SetValues:
			{
				char path[NAME_SIZE]={0};
				char value[NAME_SIZE]={0};
				validargs = sscanf(cmdline, "%s %d %s %s", cmd, &arg1, path, value);
				action.devnum = arg1;
				action.serviceType=SERVICE_CONTROL;
				action.actionType = SetValues;
				strncpy(action.paramName, path, sizeof(action.paramName)-1 );
				strncpy(action.paramValue, value, sizeof(action.paramValue)-1 );
				ret=SetValueSendAction(&action);
				if(ret<0)	printf("GetValueSendAction failed %d\n",ret);
			}
			break;	
		default:
			printf("Command not implemented; see 'Help'\n");
			break;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int rc;
	ithread_t cmdThread;
	int sig;
	sigset_t sigsCatch;
	int code;

	rc = CtrlPointStart();
	if (rc != UPNP_E_SUCCESS) {
		printf("CP start filed=%d ", rc);
		return rc;
	}

	/* start a command loop thread */
	code = ithread_create(&cmdThread,NULL,CtrlPointCommandLoop,NULL);
	if (code !=  0) {
		return UPNP_E_INTERNAL_ERROR;
	}

	/* Catch Ctrl-C and properly shutdown */
	sigemptyset(&sigsCatch);
	sigaddset(&sigsCatch, SIGINT);
	sigwait(&sigsCatch, &sig);
	printf("Shutting down on signal %d...", sig);
	rc = CtrlPointStop();

	return rc;
}

