# SSAS - Simple Smart Automotive Software
# Copyright (C) 2022 Parai Wang <parai@foxmail.com>

import xml.etree.ElementTree as ET


class ODXParser:
    def __init__(self, odxFile):
        self.root = ET.parse(odxFile).getroot()
        self.container = {}
        for n in self.root:
            if n.tag not in self.container:
                self.container[n.tag] = {}
            self.container[n.tag][n.attrib["ID"]] = self.parseNode(n)
        self.ServiceMap = {
            "SESSION": ("SESSION_CONTROL", 0x10, self.decodeSessionControl),
            "IDENTIFICATION": ("READ_DATA_BY_IDENTIFIER", 0x22, self.decodeDataIdentifier),
            "STOREDDATA": ("READ_DATA_BY_IDENTIFIER", 0x22, self.decodeDataIdentifier),
            "I/0_CONTROL": ("INPUT_OUTPUT_CONTROL_BY_IDENTIFIER", 0x2F, self.decodeIOCtrl),
        }

    def parseNode(self, node):
        nodeD = {}
        for k, v in node.attrib.items():
            if k != "ID":
                nodeD[k] = v
        for n in node:
            nD = self.parseNode(n)
            if nD is None:
                continue
            if "ID" in n.attrib.keys():
                nodeD[n.attrib["ID"]] = nD
            elif n.tag in ["DESC"]:
                nodeD[n.tag] = n.find("p").text.strip()
            elif len([x for x in n]) > 0:
                if n.tag not in nodeD:
                    nodeD[n.tag] = []
                nodeD[n.tag].append(nD)
            else:
                if n.tag in nodeD:
                    if type(nodeD[n.tag]) is list:
                        nodeD[n.tag].append(nD)
                    else:
                        nodeD[n.tag] = [nodeD[n.tag], nD]
                else:
                    nodeD[n.tag] = nD
        txt = None
        if node.text != None:
            txt = node.text.strip()
            if txt == "":
                txt = None
        if len(nodeD.keys()) == 0:
            return txt
        elif len(nodeD.keys()) == 1 and txt != None:
            k = list(nodeD.keys())[0]
            nodeD = {nodeD[k]: txt}
        else:
            nodeD["tag"] = node.tag
        return nodeD

    def decodeSessionControl(self, ser, variant):
        service = {}
        sessions = {}
        for SD in ser["SDGS"][0]["SDG"][0]["SDG"]:
            sd = {}
            for x in SD["SD"]:
                sd.update(x)
            sessions[sd["DiagInstanceQualifier"]] = {
                "ID": eval(sd["DiagInstanceStaticValue"]),
                "Name": sd["DiagInstanceName"],
            }
        service["Sessions"] = sessions
        return service

    def getDoP(self, param, variant):
        if "DIAG-CODED-TYPE" in param:
            dop = param["DIAG-CODED-TYPE"][0]
            d = {
                "BitSize": eval(dop["BIT-LENGTH"]),
                "BaseType": dop["BASE-DATA-TYPE"],
                "Encoding": dop.get("BASE-TYPE-ENCODING", None),
            }
            if "BYTE-POSITION" in param:
                d["BytePos"] = eval(param["BYTE-POSITION"]) - 3
            if "BIT-POSITION" in param:
                d["BitPos"] = eval(param["BIT-POSITION"]) - 24
            return d
        elif param["tag"] == "STRUCTURE":
            datas = []
            for x in param["PARAMS"][0]["PARAM"]:
                if "BIT-LENGTH" in x:
                    dop = {"BitSize": eval(x["BIT-LENGTH"])}
                else:
                    dop = self.getDoP(x, variant)
                d = {"Name": x["SHORT-NAME"], "Default": x.get("PHYSICAL-DEFAULT-VALUE", 0)}
                d.update(dop)
                if "BYTE-POSITION" in x:
                    d["BytePos"] = eval(x["BYTE-POSITION"])
                if "BIT-POSITION" in x:
                    d["BitPos"] = eval(x["BIT-POSITION"])
                datas.append(d)
            datas.sort(key=lambda x: self.getBitPos(x))
            return {"struct": datas}
        for ref in ["DOP-REF", "BASIC-STRUCTURE-REF"]:
            if ref in param:
                dopRef = param[ref]["ID-REF"]
                for cat in ["DATA-OBJECT-PROPS", "STRUCTURES", "DYNAMIC-LENGTH-FIELDS", "END-OF-PDU-FIELDS"]:
                    L = variant["DIAG-DATA-DICTIONARY-SPEC"][0][cat][0]
                    if dopRef in L:
                        dop = self.getDoP(L[dopRef], variant)
                        if "MAX-NUMBER-OF-ITEMS" in param:
                            dop["MaxOfItems"] = eval(param["MAX-NUMBER-OF-ITEMS"])
                        if "MIN-NUMBER-OF-ITEMS" in param:
                            dop["MinOfItems"] = eval(param["MIN-NUMBER-OF-ITEMS"])
                        return dop
        return {}

    def decodeParam(self, param, variant):
        data = {"Name": param["SHORT-NAME"], "Default": param.get("PHYSICAL-DEFAULT-VALUE", 0)}
        if "BYTE-POSITION" in param:
            data["BytePos"] = eval(param["BYTE-POSITION"]) - 3
        if "BIT-POSITION" in param:
            data["BitPos"] = eval(param["BIT-POSITION"]) - 24
        dop = self.getDoP(param, variant)
        data.update(dop)
        return data

    def getBitPos(self, data):
        BytePos = data["BytePos"]
        BitPos = data.get("BitPos", 0)
        return BytePos * 8 + (7 - BitPos)

    def decodeDataIdentifier(self, ser, variant):
        service = {}
        read = True
        ss = ser["SHORT-NAME"].split("_")
        service["Name"] = "_".join(ss[2:-1])
        service["DID"] = "0x" + ss[-1]
        for SD in ser["SDGS"][0]["SDG"]:
            sd = {}
            for x in SD["SD"]:
                sd.update(x)
            if "ServiceName" in sd:
                if sd["ServiceName"] == "Write":
                    service["Type"] = "WRITE_DATA_BY_IDENTIFIER"
                    service["ID"] = 0x2E
                    read = False
        if read:
            resRef = ser["POS-RESPONSE-REFS"][0]["POS-RESPONSE-REF"]["ID-REF"]
            posRes = variant["POS-RESPONSES"][0][resRef]
            params = posRes["PARAMS"][0]["PARAM"][2:]
        else:
            reqRef = ser["REQUEST-REF"]["ID-REF"]
            req = variant["REQUESTS"][0][reqRef]
            params = req["PARAMS"][0]["PARAM"][2:]
        service["Data"] = [self.decodeParam(p, variant) for p in params]
        service["Data"].sort(key=lambda x: self.getBitPos(x))
        return service

    def decodeIOCtrl(self, ser, variant):
        service = {}
        reqRef = ser["REQUEST-REF"]["ID-REF"]
        req = variant["REQUESTS"][0][reqRef]
        params = req["PARAMS"][0]["PARAM"][2:]
        service["Data"] = [self.decodeParam(p, variant) for p in params]
        return service

    def decodeService(self, ser, variant):
        service = {}
        ty = ser["SEMANTIC"]
        service["Type"] = ty
        sessions = []
        for SD in ser["SDGS"][0]["SDG"]:
            for x in SD["SD"]:
                if "SESSION_DIAG_LIST" in x:
                    sessions.append(x["SESSION_DIAG_LIST"])
        service["Sessions"] = sessions
        if ty in self.ServiceMap:
            ty, ID, decode = self.ServiceMap[ty]
            service["ID"] = ID
            service["Type"] = ty
            service.update(decode(ser, variant))
        return service

    def decodeServices(self, variant):
        services = {}
        for comms in variant.get("DIAG-COMMS", []):
            for ID, ser in comms.items():
                if ID == "tag":
                    continue
                service = self.decodeService(ser, variant)
                services[ser["SHORT-NAME"]] = service
        return services

    def getServices(self):
        services = {}
        for ln, layer in self.container.get("DIAG-LAYER-CONTAINER", {}).items():
            for variant in layer.get("BASE-VARIANTS", []):
                assert variant["tag"] == "BASE-VARIANTS"
                for vn, var in variant.items():
                    if vn == "tag":
                        continue
                    services[vn] = self.decodeServices(var)
        return services
