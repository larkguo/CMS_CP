#ifndef __UPNP_CTRLPT_H__
#define __UPNP_CTRLPT_H__


#ifdef __cplusplus
extern "C" {
#endif

#include "ithread.h"
#include "ixml.h" /* for IXML_Document, IXML_Element */
#include "upnp.h"
#include "UpnpString.h"
#include "upnptools.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef enum {
	STATE_UPDATE = 0,
	DEVICE_ADDED = 1,
	DEVICE_REMOVED = 2,
	GET_VAR_COMPLETE = 3
} eventType;


#define CONTROL_VARCOUNT	(6)
#define CP_MAXVARS			CONTROL_VARCOUNT
#define MAX_BUFFER	 		(2048)
#define SERVICE_SERVCOUNT	(1)
#define SERVICE_CONTROL		(0)
#define MAX_VAL_LEN			(1024)

struct Service {
    char serviceId[NAME_SIZE];
    char serviceType[NAME_SIZE];
    char *varStrVal[CP_MAXVARS];
    char eventURL[NAME_SIZE];
    char controlURL[NAME_SIZE];
    char SID[NAME_SIZE];
};

struct Device {
    char UDN[NAME_SIZE];
    char descDocURL[NAME_SIZE];
    char friendlyName[NAME_SIZE];
    char presURL[NAME_SIZE];
    int  advrTimeOut;
    struct Service service[SERVICE_SERVCOUNT];
};

struct DeviceNode {
    struct Device device;
    struct DeviceNode *next;
};

typedef struct{
	int 	devnum;
	int 	serviceType;
	int 	actionType;
	char paramName[NAME_SIZE];
	char paramValue[NAME_SIZE];
}ActionParam;

/**
* @fn int str_sub(char *st, char *orig, char *repl)
* @brief substitute a substring by another substring into a string
* @param st   : string
* @param orig : substring to replace
* @param repl : new substring
* @return the replaced string
*/
char *str_sub(const char *st, const char *orig, char *repl) ;

/**
* @fn char *Escaped(const char *st) 
* @brief unescaped a string
* @param st   : string
* @return the Eescaped string
*/
char *Escaped(const char *st) ;

/**
* @fn char *Unescaped(const char *st) 
* @brief unescaped a string
* @param st   : string
* @return the Unescaped string
*/
char *Unescaped(const char *st);

/*!
 * \brief Given a DOM node such as <Channel>11</Channel>, this routine
 * extracts the value (e.g., 11) from the node and returns it as 
 * a string. The string must be freed by the caller using free.
 *
 * \return The DOM node as a string.
 */
char *GetElementValue(
	/*! [in] The DOM node from which to extract the value. */
	IXML_Element *element);

/*!
 * \brief Given a document node, this routine searches for the first element
 * named by the input string item, and returns its value as a string.
 * String must be freed by caller using free.
 */
char *GetFirstDocumentItem(
	/*! [in] The DOM document from which to extract the value. */
	IXML_Document *doc,
	/*! [in] The item to search for. */
	const char *item); 

/*!
 * \brief Given a DOM element, this routine searches for the first element
 * named by the input string item, and returns its value as a string.
 * The string must be freed using free.
 */
char *GetFirstElementItem(
	/*! [in] The DOM element from which to extract the value. */
	IXML_Element *element,
	/*! [in] The item to search for. */
	const char *item); 

/*!
 * \brief This routine finds the first occurance of a service in a DOM
 * representation of a description document and parses it.  Note that this
 * function currently assumes that the eventURL and controlURL values in
 * the service definitions are full URLs.  Relative URLs are not handled here.
 */
int FindAndParseService (
	/*! [in] The DOM description document. */
	IXML_Document *doc,
	/*! [in] The location of the description document. */
	const char *location, 
	/*! [in] The type of service to search for. */
	const char *serviceType,
	/*! [out] The service ID. */
	char **serviceId, 
	/*! [out] The event URL for the service. */
	char **eventURL,
	/*! [out] The control URL for the service. */
	char **controlURL);

/*!
 * \brief
 */
void NotifyStateUpdate(
	/*! [in] . */
	const char *varName,
	/*! [in] . */
	const char *varValue,
	/*! [in] . */
	const char *UDN,
	/*! [in] . */
	eventType type);


/********************************************************************************
* CtrlPointDeleteNode
*
* Description: 
*       Delete a device node from the global device list.  Note that this
*       function is NOT thread safe, and should be called from another
*       function that has already locked the global device list.
*
* Parameters:
*   node -- The device node
*
********************************************************************************/
int	CtrlPointDeleteNode(struct DeviceNode *);

/********************************************************************************
* CtrlPointRemoveDevice
*
* Description: 
*       Remove a device from the global device list.
*
* Parameters:
*   UDN -- The Unique Device Name for the device to remove
*
********************************************************************************/
int	CtrlPointRemoveDevice(const char *);

/********************************************************************************
* CtrlPointRemoveAll
*
* Description: 
*       Remove all devices from the global device list.
*
* Parameters:
*   None
*
********************************************************************************/
int	CtrlPointRemoveAll(void);


/********************************************************************************
* CtrlPointRefresh
*
* Description: 
*       Clear the current global device list and issue new search
*	 requests to build it up again from scratch.
*
* Parameters:
*   None
*
********************************************************************************/
int	CtrlPointRefresh(void);

/********************************************************************************
* CtrlPointGetVar
*
* Description: 
*       Send a GetVar request to the specified service of a device.
*
* Parameters:
*   service -- The service
*   devnum -- The number of the device (order in the list,
*             starting with 1)
*   varname -- The name of the variable to request.
*
********************************************************************************/
int	CtrlPointGetVar(int, int, const char *);

/********************************************************************************
* CtrlPointGetDevice
*
* Description: 
*       Given a list number, returns the pointer to the device
*       node at that position in the global device list.  Note
*       that this function is not thread safe.  It must be called 
*       from a function that has locked the global device list.
*
* Parameters:
*   devnum -- The number of the device (order in the list,
*             starting with 1)
*   devnode -- The output device node pointer
*
********************************************************************************/
int	CtrlPointGetDevice(int, struct DeviceNode **);

/********************************************************************************
* CtrlPointPrintList
*
* Description: 
*       Print the universal device names for each device in the global device list
*
* Parameters:
*   None
*
********************************************************************************/
int	CtrlPointPrintList(void);

/********************************************************************************
* CtrlPointPrintDevice
*
* Description: 
*       Print the identifiers and state table for a device from
*       the global device list.
*
* Parameters:
*   devnum -- The number of the device (order in the list,
*             starting with 1)
*
********************************************************************************/
int	CtrlPointPrintDevice(int);


/********************************************************************************
* CtrlPointAddDevice
*
* Description: 
*       If the device is not already included in the global device list,
*       add it.  Otherwise, update its advertisement expiration timeout.
*
* Parameters:
*   doc -- The description document for the device
*   location -- The location of the description document URL
*   expires -- The expiration time for this advertisement
*
********************************************************************************/
void	CtrlPointAddDevice(IXML_Document *, const char *, int); 

void  CtrlPointHandleGetVar(const char *, const char *, const DOMString);

/*!
 * \brief Update a state table. Called when an event is received.
 *
 * Note: this function is NOT thread save. It must be called from another
 * function that has locked the global device list.
 **/
void StateVarUpdate(
	/*! [in] The UDN of the parent device. */
	char *UDN,
	/*! [in] The service state table to update. */
	int service,
	/*! [out] DOM document representing the XML received with the event. */
	IXML_Document *changedVariables,
	/*! [out] pointer to the state table for the  service to update. */
	char **state);


/********************************************************************************
* CtrlPointHandleEvent
*
* Description: 
*       Handle a UPnP event that was received.  Process the event and update
*       the appropriate service state table.
*
* Parameters:
*   sid -- The subscription id for the event
*   eventkey -- The eventkey number for the event
*   changes -- The DOM document representing the changes
*
********************************************************************************/
void	CtrlPointHandleEvent(const char *, int, IXML_Document *); 

/********************************************************************************
* CtrlPointHandleSubscribeUpdate
*
* Description: 
*       Handle a UPnP subscription update that was received.  Find the 
*       service the update belongs to, and update its subscription
*       timeout.
*
* Parameters:
*   eventURL -- The event URL for the subscription
*   sid -- The subscription id for the subscription
*   timeout  -- The new timeout for the subscription
*
********************************************************************************/
void	CtrlPointHandleSubscribeUpdate(const char *, const Upnp_SID, int); 


/********************************************************************************
* CtrlPointCallbackEventHandler
*
* Description: 
*       The callback handler registered with the SDK while registering
*       the control point.  Detects the type of callback, and passes the 
*       request on to the appropriate function.
*
* Parameters:
*   eventType -- The type of callback event
*   event -- Data structure containing event data
*   cookie -- Optional data specified during callback registration
*
********************************************************************************/
int	CtrlPointCallbackEventHandler(Upnp_EventType, void *, void *);

/*!
 * \brief Checks the advertisement each device in the global device list.
 *
 * If an advertisement expires, the device is removed from the list.
 *
 * If an advertisement is about to expire, a search request is sent for that
 * device.
 */
void CtrlPointVerifyTimeouts(
	/*! [in] The increment to subtract from the timeouts each time the
	 * function is called. */
	int incr);

void* CtrlPointCommandLoop(void *);

/*!
* \brief Function that runs in its own thread and monitors advertisement
* and subscription timeouts for devices in the global device list.
*/
void *CtrlPointTimerLoop(void *args);

/*!
* \brief Call this function to initialize the UPnP library and start the CMS
* Control Point.  This function creates a timer thread and provides a
* callback handler to process any UPnP events that are received.
*
* \return 0 if everything went well, else -1.
*/
int	CtrlPointStart();
int	CtrlPointStop(void);
int	CtrlPointProcessCommand(char *cmdline);
void CtrlPointPrintHelp(void);


/*!
 * \brief Function that receives commands from the user at the command prompt
 * during the lifetime of the device, and calls the appropriate
 * functions for those commands.
 */
void *CtrlPointCommandLoop(void *args);

void PrintParameters(const char *buffer);

#ifdef __cplusplus
};
#endif

#endif

