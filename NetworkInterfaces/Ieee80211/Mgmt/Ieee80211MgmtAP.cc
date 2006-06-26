//
// Copyright (C) 2006 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//


#include "Ieee80211MgmtAP.h"
#include "Ieee802Ctrl_m.h"
#include "EtherFrame_m.h"


Define_Module(Ieee80211MgmtAP);

static std::ostream& operator<< (std::ostream& os, const Ieee80211MgmtAP::STAInfo& sta)
{
    os << "state:" << sta.status;
    return os;
}

void Ieee80211MgmtAP::initialize(int stage)
{
    Ieee80211MgmtAPBase::initialize(stage);

    if (stage==0)
    {
        // read params and init vars
        ssid = par("ssid").stringValue();
        channelNumber = par("channelNumber");
        beaconInterval = par("beaconInterval");
        numAuthSteps = par("numAuthSteps");
        if (numAuthSteps!=2 && numAuthSteps!=4)
            error("parameter 'numAuthSteps' (number of frames exchanged during authentication) must be 2 or 4, not %d", numAuthSteps);
        WATCH(ssid);
        WATCH(channelNumber);
        WATCH(beaconInterval);
        WATCH(numAuthSteps);
        WATCH_MAP(staList);

        //TBD fill in supportedRates and capabilityInfo
        //TBD tune MAC to the given channel

        // start beacon timer (randomize startup time)
        beaconTimer = new cMessage("beaconTimer");
        scheduleAt(uniform(0,beaconInterval), beaconTimer);
    }
}

void Ieee80211MgmtAP::handleTimer(cMessage *msg)
{
    if (msg==beaconTimer)
    {
        sendBeacon();
        scheduleAt(simTime()+beaconInterval, beaconTimer);
    }
    else
    {
        error("internal error: unrecognized timer '%s'", msg->name());
    }
}

void Ieee80211MgmtAP::handleUpperMessage(cMessage *msg)
{
    // must be an EtherFrame frame arriving from MACRelayUnit, that is,
    // bridged from another interface of the AP (probably Ethernet).
    EtherFrame *etherframe = check_and_cast<EtherFrame *>(msg);

    // check we really have a STA with that dest address
    STAList::iterator it = staList.find(etherframe->getDest());
    if (it==staList.end() || it->second.status!=ASSOCIATED)
    {
        EV << "STA with MAC address " << etherframe->getDest() << " not associated with this AP, dropping frame\n";
        delete etherframe; // XXX count drops?
        return;
    }

    // convert Ethernet frame
    Ieee80211DataFrame *frame = convertFromEtherFrame(etherframe);
    sendOrEnqueue(frame);
}

void Ieee80211MgmtAP::receiveChangeNotification(int category, cPolymorphic *details)
{
    Enter_Method_Silent();
    // ignore notifications
}

Ieee80211MgmtAP::STAInfo *Ieee80211MgmtAP::lookupSenderSTA(Ieee80211ManagementFrame *frame)
{
    STAList::iterator it = staList.find(frame->getTransmitterAddress());
    return it==staList.end() ? NULL : &(it->second);
}

void Ieee80211MgmtAP::sendManagementFrame(Ieee80211ManagementFrame *frame, const MACAddress& destAddr)
{
    frame->setFromDS(true);
    frame->setReceiverAddress(destAddr);
    frame->setAddress3(myAddress);
    sendOrEnqueue(frame); //FIXME or do mgmt frames have priority?
}

void Ieee80211MgmtAP::sendBeacon()
{
    Ieee80211BeaconFrame *frame = new Ieee80211BeaconFrame("Beacon");
    Ieee80211BeaconFrameBody& body = frame->getBody();
    body.setSSID(ssid.c_str());
    body.setSupportedRates(supportedRates);
    body.setCapabilityInformation(capabilityInfo);
    body.setTimestamp(simTime()); //XXX this is to be refined
    body.setBeaconInterval(beaconInterval);
    body.setDSChannel(channelNumber);

    frame->setReceiverAddress(MACAddress::BROADCAST_ADDRESS);  //XXX or what
    frame->setFromDS(true);

    sendOrEnqueue(frame); // FIXME it's not that simple! must insert at front of the queue,
                          // plus there are special timing requirements...
                          // FIXME and MAC should synchronize to beacon timing?
}

void Ieee80211MgmtAP::handleDataFrame(Ieee80211DataFrame *frame)
{
    // check toDS bit
    if (!frame->getToDS())
    {
        // looks like this is not for us - discard
        delete frame;
        return;
    }

    // look up destination address in our STA list
    STAList::iterator it = staList.find(frame->getAddress3());
    if (it==staList.end())
    {
        // not our STA -- pass up frame to relayUnit for LAN bridging if we have one
        if (hasRelayUnit)
            send(createEtherFrame(frame), "uppergateOut");
        else
            delete frame;
    }
    else
    {
        // dest address is our STA, but is it already associated?
        if (it->second.status == ASSOCIATED)
            distributeReceivedDataFrame(frame); // send it out to the destination STA
        else
            delete frame;
    }
}

void Ieee80211MgmtAP::handleAuthenticationFrame(Ieee80211AuthenticationFrame *frame)
{
    // create STA entry if needed
    STAInfo *sta = lookupSenderSTA(frame);
    if (!sta)
    {
        MACAddress staAddress = frame->getTransmitterAddress();
        sta = &staList[staAddress]; // this implicitly creates a new entry
        sta->address = staAddress;
        sta->status = NOT_AUTHENTICATED;
        sta->authSeqExpected = 1;
    }

    // check authentication sequence number is OK
    int frameAuthSeq = frame->getBody().getSequenceNumber();
    if (frameAuthSeq != sta->authSeqExpected)
    {
        // wrong sequence number: send error and return
        Ieee80211AuthenticationFrame *resp = new Ieee80211AuthenticationFrame("Auth(ERR)");
        resp->getBody().setStatusCode(SC_AUTH_OUT_OF_SEQ);
        sendManagementFrame(resp, frame->getTransmitterAddress());
        delete frame;
        sta->authSeqExpected = 1; // go back to start square
        return;
    }

    // send OK response (we don't model the cryptography part, just assume
    // successful authentication every time)
    Ieee80211AuthenticationFrame *resp = new Ieee80211AuthenticationFrame("Auth(OK)");
    resp->getBody().setSequenceNumber(frameAuthSeq);
    resp->getBody().setStatusCode(SC_SUCCESSFUL);
    // XXX frame length could be increased to account for challenge text length etc.
    sendManagementFrame(resp, frame->getTransmitterAddress());

    // station is authenticated if it made it through the required number of steps
    if (frameAuthSeq+1 == numAuthSteps)
    {
        sta->status = AUTHENTICATED; // XXX maybe only when ACK of this frame arrives?
    }
    else
    {
        sta->authSeqExpected += 2;
    }
}

void Ieee80211MgmtAP::handleDeauthenticationFrame(Ieee80211DeauthenticationFrame *frame)
{
    STAInfo *sta = lookupSenderSTA(frame);
    delete frame;

    if (sta)
    {
        // mark STA as not authenticated; alternatively, it could also be removed from staList
        sta->status = NOT_AUTHENTICATED;
        sta->authSeqExpected = 1;
    }
}

void Ieee80211MgmtAP::handleAssociationRequestFrame(Ieee80211AssociationRequestFrame *frame)
{
    // "11.3.2 AP association procedures"
    STAInfo *sta = lookupSenderSTA(frame);
    if (!sta || sta->status==NOT_AUTHENTICATED)
    {
        // STA not authenticated: send error and return
        Ieee80211DeauthenticationFrame *resp = new Ieee80211DeauthenticationFrame("Deauth");
        resp->getBody().setReasonCode(RC_NONAUTH_ASS_REQUEST);
        sendManagementFrame(resp, frame->getTransmitterAddress());
        delete frame;
        return;
    }

    // mark STA as associated
    sta->status = ASSOCIATED; // XXX this should only take place when MAC receives the ACK for the response

    // send OK response
    Ieee80211AssociationResponseFrame *resp = new Ieee80211AssociationResponseFrame("AssocResp(OK)");
    Ieee80211AssociationResponseFrameBody& body = resp->getBody();
    body.setStatusCode(SC_SUCCESSFUL);
    body.setCapabilityInformation(capabilityInfo);
    body.setAid(0); //XXX
    body.setSupportedRates(supportedRates);
    sendManagementFrame(resp, sta->address);
}

void Ieee80211MgmtAP::handleAssociationResponseFrame(Ieee80211AssociationResponseFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211MgmtAP::handleReassociationRequestFrame(Ieee80211ReassociationRequestFrame *frame)
{
    // "11.3.4 AP reassociation procedures" -- almost the same as AssociationRequest processing
    STAInfo *sta = lookupSenderSTA(frame);
    if (!sta || sta->status==NOT_AUTHENTICATED)
    {
        // STA not authenticated: send error and return
        Ieee80211DeauthenticationFrame *resp = new Ieee80211DeauthenticationFrame("Deauth");
        resp->getBody().setReasonCode(RC_NONAUTH_ASS_REQUEST);
        sendManagementFrame(resp, frame->getTransmitterAddress());
        delete frame;
        return;
    }

    // mark STA as associated
    sta->status = ASSOCIATED; // XXX this should only take place when MAC receives the ACK for the response

    // send OK response
    Ieee80211ReassociationResponseFrame *resp = new Ieee80211ReassociationResponseFrame("ReassocResp(OK)");
    Ieee80211ReassociationResponseFrameBody& body = resp->getBody();
    body.setStatusCode(SC_SUCCESSFUL);
    body.setCapabilityInformation(capabilityInfo);
    body.setAid(0); //XXX
    body.setSupportedRates(supportedRates);
    sendManagementFrame(resp, sta->address);
}

void Ieee80211MgmtAP::handleReassociationResponseFrame(Ieee80211ReassociationResponseFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211MgmtAP::handleDisassociationFrame(Ieee80211DisassociationFrame *frame)
{
    STAInfo *sta = lookupSenderSTA(frame);
    delete frame;

    if (sta)
    {
        sta->status = AUTHENTICATED;
    }
}

void Ieee80211MgmtAP::handleBeaconFrame(Ieee80211BeaconFrame *frame)
{
    dropManagementFrame(frame);
}

void Ieee80211MgmtAP::handleProbeRequestFrame(Ieee80211ProbeRequestFrame *frame)
{
    if (strcmp(frame->getBody().getSSID(), ssid.c_str())!=0)
    {
        EV << "SSID does not match, ignoring frame\n";
        dropManagementFrame(frame);
        return;
    }

    MACAddress staAddress = frame->getTransmitterAddress();
    delete frame;

    Ieee80211ProbeResponseFrame *resp = new Ieee80211ProbeResponseFrame("ProbeResp");
    Ieee80211ProbeResponseFrameBody& body = resp->getBody();
    body.setSSID(ssid.c_str());
    body.setSupportedRates(supportedRates);
    body.setCapabilityInformation(capabilityInfo);
    body.setTimestamp(simTime()); //XXX this is to be refined
    body.setBeaconInterval(beaconInterval);
    body.setDSChannel(channelNumber);
    sendManagementFrame(resp, staAddress); // FIXME it might be not that simple, cf beacon...
}

void Ieee80211MgmtAP::handleProbeResponseFrame(Ieee80211ProbeResponseFrame *frame)
{
    dropManagementFrame(frame);
}

