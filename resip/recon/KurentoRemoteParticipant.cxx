#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <kurento-client/KurentoClient.h>

#include "KurentoConversationManager.hxx"

#include "KurentoRemoteParticipant.hxx"
#include "Conversation.hxx"
#include "UserAgent.hxx"
#include "DtmfEvent.hxx"
#include "ReconSubsystem.hxx"

#include <rutil/Log.hxx>
#include <rutil/Logger.hxx>
#include <rutil/DnsUtil.hxx>
#include <rutil/Random.hxx>
#include <resip/stack/DtmfPayloadContents.hxx>
#include <resip/stack/SipFrag.hxx>
#include <resip/stack/ExtensionHeader.hxx>
#include <resip/dum/DialogUsageManager.hxx>
#include <resip/dum/ClientInviteSession.hxx>
#include <resip/dum/ServerInviteSession.hxx>
#include <resip/dum/ClientSubscription.hxx>
#include <resip/dum/ServerOutOfDialogReq.hxx>
#include <resip/dum/ServerSubscription.hxx>

#include <rutil/WinLeakCheck.hxx>

#include <utility>

using namespace recon;
using namespace resip;
using namespace std;

#define RESIPROCATE_SUBSYSTEM ReconSubsystem::RECON

/* Technically, there are a range of features that need to be implemented
   to be fully (S)AVPF compliant.
   However, it is speculated that (S)AVPF peers will communicate with legacy
   systems that just fudge the RTP/SAVPF protocol in their SDP.  Enabling
   this define allows such behavior to be tested. 

   http://www.ietf.org/mail-archive/web/rtcweb/current/msg01145.html
   "1) RTCWEB end-point will always signal AVPF or SAVPF. I signalling
   gateway to legacy will change that by removing the F to AVP or SAVP."

   http://www.ietf.org/mail-archive/web/rtcweb/current/msg04380.html
*/
//#define RTP_SAVPF_FUDGE

// UAC
KurentoRemoteParticipant::KurentoRemoteParticipant(ParticipantHandle partHandle,
                                     KurentoConversationManager& kurentoConversationManager,
                                     DialogUsageManager& dum,
                                     RemoteParticipantDialogSet& remoteParticipantDialogSet)
: Participant(partHandle, kurentoConversationManager),
  RemoteParticipant(partHandle, kurentoConversationManager, dum, remoteParticipantDialogSet),
  KurentoParticipant(partHandle, kurentoConversationManager)
{
   InfoLog(<< "KurentoRemoteParticipant created (UAC), handle=" << mHandle);
}

// UAS - or forked leg
KurentoRemoteParticipant::KurentoRemoteParticipant(KurentoConversationManager& kurentoConversationManager,
                                     DialogUsageManager& dum, 
                                     RemoteParticipantDialogSet& remoteParticipantDialogSet)
: Participant(kurentoConversationManager),
  RemoteParticipant(kurentoConversationManager, dum, remoteParticipantDialogSet),
  KurentoParticipant(kurentoConversationManager)
{
   InfoLog(<< "KurentoRemoteParticipant created (UAS or forked leg), handle=" << mHandle);
}

KurentoRemoteParticipant::~KurentoRemoteParticipant()
{
   // Note:  Ideally this call would exist in the Participant Base class - but this call requires 
   //        dynamic_casts and virtual methods to function correctly during destruction.
   //        If the call is placed in the base Participant class then these things will not
   //        function as desired because a classes type changes as the descructors unwind.
   //        See https://stackoverflow.com/questions/10979250/usage-of-this-in-destructor.
   unregisterFromAllConversations();

   InfoLog(<< "KurentoRemoteParticipant destroyed, handle=" << mHandle);
}

int 
KurentoRemoteParticipant::getConnectionPortOnBridge()
{
   if(getDialogSet().getActiveRemoteParticipantHandle() == mHandle)
   {
      return -1;  // FIXME Kurento
   }
   else
   {
      // If this is not active fork leg, then we don't want to effect the bridge mixer.  
      // Note:  All forked endpoints/participants have the same connection port on the bridge
      return -1;
   }
}

int 
KurentoRemoteParticipant::getMediaConnectionId()
{ 
   return getKurentoDialogSet().getMediaConnectionId();
}

void
KurentoRemoteParticipant::buildSdpOffer(bool holdSdp, SdpContents& offer)
{
   // FIXME Kurento - this needs to be async

   // FIXME Kurento - use holdSdp

   // FIXME Kurento - include video, SRTP, WebRTC?

   try
   {
      kurento_client::KurentoClient& client = mKurentoConversationManager.getKurentoClient();

      client.createMediaPipeline();
      client.createRtpEndpoint();
      client.invokeConnect();
      client.invokeProcessOffer();
      client.describeObject();

      std::string _offer(client.getMReturnedSdp());

      StackLog(<<"offer FROM Kurento: " << _offer.c_str());

      ParseBuffer pb(_offer.c_str(), _offer.size());
      offer.parse(pb);

      setProposedSdp(offer);
   }
   catch(exception& e)
   {
      ErrLog(<<"something went wrong: " << e.what());
   }
}

bool
KurentoRemoteParticipant::buildSdpAnswer(const SdpContents& offer, SdpContents& answer)
{
   // FIXME Kurento - this needs to be async

   bool valid = false;

   try
   {
      // do some checks on the offer
      // check for video, check for WebRTC
      typedef std::map<resip::Data, int> MediaSummary;
      MediaSummary mediaMap;
      std::set<resip::Data> mediumTransports;
      for(SdpContents::Session::MediumContainer::const_iterator it = offer.session().media().cbegin();
               it != offer.session().media().cend();
               it++)
      {
         const SdpContents::Session::Medium& m = *it;
         if(mediaMap.find(m.name()) != mediaMap.end())
         {
            mediaMap[m.name()]++;
         }
         else
         {
            mediaMap[m.name()] = 1;
         }
         mediumTransports.insert(m.protocol());
      }
      for(MediaSummary::const_iterator it = mediaMap.begin();
               it != mediaMap.end();
               it++)
      {
         StackLog(<<"found medium type " << it->first << " "  << it->second << " instance(s)");
      }
      for(std::set<resip::Data>::const_iterator it = mediumTransports.cbegin();
               it != mediumTransports.end();
               it++)
      {
         StackLog(<<"found protocol " << *it);
      }

      std::ostringstream _offer;
      _offer << offer;

      StackLog(<<"offer TO Kurento: " << _offer.str());

      kurento_client::KurentoClient& client = mKurentoConversationManager.getKurentoClient();

      client.getMConnectionHandler()->createConnection("127.0.0.1", "8888");

      client.setMSdp(_offer.str());
      client.createMediaPipeline();
      client.createRtpEndpoint();
      client.invokeConnect();
      client.invokeProcessOffer();
      client.describeObject();

      std::string _answer(client.getMReturnedSdp());

      StackLog(<<"answer FROM Kurento: " << _answer);

      ParseBuffer pb(_answer.c_str(), _answer.size());
      answer.parse(pb);

      valid = true;
   }
   catch(exception& e)
   {
      ErrLog(<<"something went wrong: " << e.what());
   }

   if(valid)
   {
      setLocalSdp(answer);
      setRemoteSdp(offer);
   }

   return valid;
}

void
KurentoRemoteParticipant::adjustRTPStreams(bool sendingOffer)
{
   // FIXME Kurento - implement, may need to break up this method into multiple parts

   // FIXME Kurento - sometimes true
   setRemoteHold(false);

}

bool
KurentoRemoteParticipant::mediaStackPortAvailable()
{
   return true; // FIXME Kurento - can we check with Kurento somehow?
}


/* ====================================================================

 Copyright (c) 2021, SIP Spectrum, Inc.
 Copyright (c) 2021, Daniel Pocock https://danielpocock.com
 Copyright (c) 2007-2008, Plantronics, Inc.
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are 
 met:

 1. Redistributions of source code must retain the above copyright 
    notice, this list of conditions and the following disclaimer. 

 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution. 

 3. Neither the name of Plantronics nor the names of its contributors 
    may be used to endorse or promote products derived from this 
    software without specific prior written permission. 

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 ==================================================================== */
