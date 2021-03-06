
/**
 * libemulation
 * OEDocument
 * (C) 2010-2012 by Marc S. Ressl (mressl@umich.edu)
 * Released under the GPL
 *
 * Controls an emulator XML description
 */

#ifndef _OEDOCUMENT_H
#define _OEDOCUMENT_H

#include <libxml/tree.h>

#include "OECommon.h"
#include "OEPackage.h"

#define OE_FILE_PATH_EXTENSION "xml"
#define OE_PACKAGE_PATH_EXTENSION "emulation"
#define OE_PACKAGE_EDL_PATH "info.xml"

typedef struct
{
    string label;
    string image;
    string description;
} OEHeaderInfo;

typedef struct
{
    string id;
    string ref;
    string type;
    string group;
    string label;
    string image;
} OEPortInfo;

typedef struct
{
    string id;
    string type;
} OEConnectorInfo;

typedef vector<OEPortInfo> OEPortInfos;
typedef vector<OEConnectorInfo> OEConnectorInfos;

typedef vector<string> OEIds;
typedef map<string, string> OEIdMap;
typedef map<string, OEIdMap> OEInletMap;

string OESetDeviceId(string id, string deviceId);
string OEGetDeviceId(string id);

class OEDocument
{
public:
    OEDocument();
    ~OEDocument();
    
    bool open(string path);
    bool isOpen();
    bool save(string path);
    void close();
    
    OEHeaderInfo getHeaderInfo();
    OEPortInfos getPortInfos();
    OEConnectorInfos getFreeConnectorInfos();
    
    bool addDocument(string path, OEIdMap connections);
    bool removeDevice(string deviceId);
    
    OEIds getDeviceIds();
    
protected:
    bool is_open;
    OEPackage *package;
    xmlDocPtr doc;
    
    virtual bool constructDocument(xmlDocPtr doc);
    virtual bool configureInlets(OEInletMap& inletMap);
    virtual bool reconfigureDocument(xmlDocPtr doc);
    virtual void disposeDevice(string deviceId);
    virtual void deconfigureDevice(string deviceId);
    virtual void destroyDevice(string deviceId);
    
    string getLocationLabel(string deviceId, vector<string>& visitedIds);
    string getLocationLabel(string deviceId);
    
    string getNodeName(xmlNodePtr node);
    string getNodeProperty(xmlNodePtr node, string name);
    bool hasNodeProperty(xmlNodePtr node, string name);
    void setNodeProperty(xmlNodePtr node, string name, string value);
    
private:
    bool validateDocument();
    bool dumpDocument(OEData& data);
    
    xmlDocPtr getXMLDoc();
    OEIdMap makeIdMap(OEIds& deviceIds);
    void remapNodeProperty(OEIdMap& deviceIdMap, xmlNodePtr node, string property);
    void remapDocument(OEIdMap& deviceIdMap);
    void remapConnections(OEIdMap& deviceIdMap, OEIdMap& connections);
    xmlNodePtr getLastNode(string theDeviceId);
    string followDeviceChain(string deviceId, vector<string>& visitedIds);
    xmlNodePtr getInsertionNode(string connectorId);
    bool insertInto(xmlNodePtr dest);
    void addInlets(OEInletMap& inletMap, string deviceId, xmlNodePtr children);
    OEInletMap getInlets(xmlDocPtr doc, OEIdMap& connections, string nodeType);
    void connectPorts(xmlDocPtr doc, OEIdMap& connections);
    void connectInlets(xmlDocPtr doc, OEInletMap& inletMap);
    void connectInlet(OEIdMap& propertyMap, xmlNodePtr children);
    void disconnectDevice(string deviceId);
    void deleteDevice(string deviceId);
};

#endif
