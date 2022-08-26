#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <regex>

#include "GstMediaStackAdapter.hxx"

#include "GstRemoteParticipant.hxx"
#include "Conversation.hxx"
#include "UserAgent.hxx"
#include "DtmfEvent.hxx"
#include "ReconSubsystem.hxx"

#include <rutil/Log.hxx>
#include <rutil/Logger.hxx>
#include <rutil/DnsUtil.hxx>
#include <rutil/Random.hxx>
#include <resip/stack/DtmfPayloadContents.hxx>
#include <resip/stack/SdpContents.hxx>
#include <resip/stack/SipFrag.hxx>
#include <resip/stack/ExtensionHeader.hxx>
#include <resip/dum/DialogUsageManager.hxx>
#include <resip/dum/ClientInviteSession.hxx>
#include <resip/dum/ServerInviteSession.hxx>
#include <resip/dum/ClientSubscription.hxx>
#include <resip/dum/ServerOutOfDialogReq.hxx>
#include <resip/dum/ServerSubscription.hxx>

#include <rutil/WinLeakCheck.hxx>
#include <media/gstreamer/GStreamerUtils.hxx>
#include <media/gstreamer/GstRtpManager.hxx>

#include <reflow/HEPRTCPEventLoggingHandler.hxx>

#include <memory>
#include <utility>

#include <glibmm/error.h>
#include <glibmm/signalproxy.h>
#include <gst/gst.h>
#include <gst/gsterror.h>
#include <gst/sdp/gstsdpmessage.h>
#include <gstreamermm/pipeline.h>
//#include <gstreamermm/promise.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

using namespace resipgst;
using namespace recon;
using namespace resip;
using namespace std;

using namespace Gst;
using Glib::RefPtr;

#define RESIPROCATE_SUBSYSTEM ReconSubsystem::RECON

/* The demo code for webrtcbin provides a useful example.
 *
 * This appears to be the current location of the demos:
 *
 * https://gitlab.freedesktop.org/gstreamer/gstreamer/-/tree/main/subprojects/gst-examples/webrtc
 *
 * Previous locations:
 *
 * https://github.com/centricular/gstwebrtc-demos
 * https://github.com/imdark/gstreamer-webrtc-demo
 */


// These are currently used as defaults for creating an SDP offer for WebRTC
#define GST_RTP_CAPS_OPUS "application/x-rtp,media=audio,encoding-name=OPUS,payload=96"
#define GST_RTP_CAPS_VP8 "application/x-rtp,media=video,encoding-name=VP8,payload=97"

// used with g_signal_connect
void voidStub(GstElement * webrtcbin, void *u)
{
   StackLog(<<"voidStub invoked");
   std::function<void()> &func = *static_cast<std::function<void()>*>(u);
   func();
}
/*void propertyStateStub(GstElement * webrtcbin, GParamSpec * pspec,
   void *u)
{
   std::function<void(GParamSpec *)> &func = *static_cast<std::function<void(GParamSpec *)>*>(u);
   func(pspec);
}*/

void promise_stub(GstPromise* promise, void *u) {
  StackLog(<<"promise_stub invoked");
  // context is static_cast<void*>(&f) below. We reverse the cast to
  // void* and call the std::function object.
  auto _u = static_cast<std::function<void(GstPromise* , gpointer)>*>(u);
  std::function<void(GstPromise*, gpointer)> &func = *_u;
  func(promise, NULL);
  delete _u;  // each promise has a copy of the function object that must be deleted
}

// UAC
GstRemoteParticipant::GstRemoteParticipant(ParticipantHandle partHandle,
                                     ConversationManager& conversationManager,
                                     GstMediaStackAdapter& gstreamerMediaStackAdapter,
                                     DialogUsageManager& dum,
                                     RemoteParticipantDialogSet& remoteParticipantDialogSet)
: Participant(partHandle, ConversationManager::ParticipantType_Remote, conversationManager),
  RemoteParticipant(partHandle, conversationManager, dum, remoteParticipantDialogSet),
  GstParticipant(partHandle, ConversationManager::ParticipantType_Remote, conversationManager, gstreamerMediaStackAdapter),
  mIceGatheringDone(false),
  mRemoveExtraMediaDescriptors(false),
  mSipRtpEndpoint(true),
  mReuseSdpAnswer(false),
  mWSAcceptsKeyframeRequests(true),
  mLastRemoteSdp(0),
  mWaitingAnswer(false),
  mWebRTCOutgoing(getDialogSet().getConversationProfile()->mediaEndpointMode() == ConversationProfile::WebRTC)
{
   InfoLog(<< "GstRemoteParticipant created (UAC), handle=" << mHandle);
}

// UAS - or forked leg
GstRemoteParticipant::GstRemoteParticipant(ConversationManager& conversationManager,
                                     GstMediaStackAdapter& gstreamerMediaStackAdapter,
                                     DialogUsageManager& dum, 
                                     RemoteParticipantDialogSet& remoteParticipantDialogSet)
: Participant(ConversationManager::ParticipantType_Remote, conversationManager),
  RemoteParticipant(conversationManager, dum, remoteParticipantDialogSet),
  GstParticipant(ConversationManager::ParticipantType_Remote, conversationManager, gstreamerMediaStackAdapter),
  mIceGatheringDone(false),
  mRemoveExtraMediaDescriptors(false),
  mSipRtpEndpoint(true),
  mReuseSdpAnswer(false),
  mWSAcceptsKeyframeRequests(true),
  mLastRemoteSdp(0),
  mWaitingAnswer(false),
  mWebRTCOutgoing(getDialogSet().getConversationProfile()->mediaEndpointMode() == ConversationProfile::WebRTC)
{
   InfoLog(<< "GstRemoteParticipant created (UAS or forked leg), handle=" << mHandle);
}

GstRemoteParticipant::~GstRemoteParticipant()
{
   // Note:  Ideally this call would exist in the Participant Base class - but this call requires 
   //        dynamic_casts and virtual methods to function correctly during destruction.
   //        If the call is placed in the base Participant class then these things will not
   //        function as desired because a classes type changes as the descructors unwind.
   //        See https://stackoverflow.com/questions/10979250/usage-of-this-in-destructor.
   unregisterFromAllConversations();

   mPipeline->set_state(STATE_NULL);

   InfoLog(<< "GstRemoteParticipant destroyed, handle=" << mHandle);
}

int 
GstRemoteParticipant::getConnectionPortOnBridge()
{
   if(getDialogSet().getActiveRemoteParticipantHandle() == mHandle)
   {
      return -1;  // FIXME Gst
   }
   else
   {
      // If this is not active fork leg, then we don't want to effect the bridge mixer.  
      // Note:  All forked endpoints/participants have the same connection port on the bridge
      return -1;
   }
}

int 
GstRemoteParticipant::getMediaConnectionId()
{ 
   return getGstDialogSet().getMediaConnectionId();
}

void
GstRemoteParticipant::applyBridgeMixWeights()
{
   // FIXME Gst - do we need to implement this?
}

// Special version of this call used only when a participant
// is removed from a conversation.  Required when sipXConversationMediaInterfaceMode
// is used, in order to get a pointer to the bridge mixer
// for a participant (ie. LocalParticipant) that has no currently
// assigned conversations.
void
GstRemoteParticipant::applyBridgeMixWeights(Conversation* removedConversation)
{
   // FIXME Gst - do we need to implement this?
}

void
GstRemoteParticipant::linkOutgoingPipeline(unsigned int streamId, RefPtr<Bin> bin, const Glib::RefPtr<Gst::Caps> caps)
{
   RefPtr<Pad> src = bin->get_static_pad("src");

   RefPtr<Caps> padCaps = Caps::create_from_string("application/x-rtp");
   RefPtr<Gst::PadTemplate> tmpl = PadTemplate::create("send_rtp_sink_%u", PAD_SINK, PAD_REQUEST, padCaps);
   ostringstream padName;
   padName << "sink_" << streamId;
   DebugLog(<< "linking to " << padName.str());
   //RefPtr<Pad> binSink = mRtpTransportElement->create_compatible_pad(queueSrc, caps);
   //RefPtr<Pad> binSink = mRtpTransportElement->get_request_pad(padName.str());
   //RefPtr<Caps> padCaps2 = Caps::create_from_string("application/x-rtp");
   //RefPtr<Pad> binSink = mRtpTransportElement->request_pad(tmpl, padName.str(), padCaps2);
   // is a ghost pad, should have been created already

   // it is static_pad on the rtpbin wrapper
   RefPtr<Pad> binSink;
   bool isWebRTC = isWebRTCSession();
   if(isWebRTC)
   {
      // and it is a request pad on the webrtcbin
      binSink = mRtpTransportElement->create_compatible_pad(src, caps);
   }
   else
   {
      binSink = mRtpTransportElement->get_static_pad(padName.str());
   }
   if(!binSink)
   {
      CritLog(<<"failed to get request pad " << padName.str());
      resip_assert(0);
   }

   src->link(binSink);

   debugGraph();
}

void
sendCapsEvent(RefPtr<Bin> bin, RefPtr<Caps> caps)
{
   // webrtcbin seems to expect this
   // https://gitlab.freedesktop.org/gstreamer/gst-plugins-bad/-/issues/1055#note_631630

   // send RTP caps into the sink towards the webrtcbin transport bin
   RefPtr<Gst::EventCaps> capsEvent = Gst::EventCaps::create(caps);
   RefPtr<Gst::Element> queueOut = RefPtr<Element>::cast_dynamic(bin->get_child("queueOut"));
   queueOut->get_static_pad("sink")->send_event(capsEvent);

   // send audio/raw-something caps into the bin
   //RefPtr<Gst::EventCaps> capsEvent = Gst::EventCaps::create(___caps);
   //sink->send_event(capsEvent);
}

bool
GstRemoteParticipant::initEndpointIfRequired(bool isWebRTC)
{
   if(mPipeline)
   {
      DebugLog(<<"mPipeline already exists");
      return false;
   }
   if(!isWebRTC)
   {
      // skip the ICE phase as we don't do ICE for non-WebRTC yet
      mIceGatheringDone = true;
   }
   else
   {
      mIceGatheringDone = false;
   }

   mPipeline = Gst::Pipeline::create();
   if(isWebRTC)
   {
      mRtpTransportElement = Gst::ElementFactory::create_element("webrtcbin");

      // FIXME: use a glibmm-style property
      // FIXME: per-peer bundle-policy
      // FIXME: move to config file
      // https://datatracker.ietf.org/doc/html/draft-ietf-rtcweb-jsep-24#section-4.1.1
      // https://gstreamer.freedesktop.org/documentation/webrtc/index.html?gi-language=c#webrtcbin:bundle-policy
      //GstWebRTCBundlePolicy bundlePolicy = GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE;
      GstWebRTCBundlePolicy bundlePolicy = GST_WEBRTC_BUNDLE_POLICY_MAX_COMPAT;
      GValue val = G_VALUE_INIT;
      g_value_init (&val, G_TYPE_ENUM);
      g_value_set_enum (&val, bundlePolicy);
      g_object_set_property(G_OBJECT(mRtpTransportElement->gobj()), "bundle-policy", &val);

      std::shared_ptr<resip::ConfigParse> cfg = mConversationManager.getConfig();
      if(cfg)
      {
         // FIXME: support TURN too
         const Data& natTraversalMode = mConversationManager.getConfig()->getConfigData("NatTraversalMode", "", true);
         const Data& stunServerName = mConversationManager.getConfig()->getConfigData("NatTraversalServerHostname", "", true);
         const int stunServerPort = mConversationManager.getConfig()->getConfigInt("NatTraversalServerPort", 3478);
         if(natTraversalMode == "Bind" && !stunServerName.empty() && stunServerPort > 0)
         {
            Data stunUrl = "stun://" + stunServerName + ":" + stunServerPort;
            InfoLog(<<"using STUN config: " << stunUrl);
            mRtpTransportElement->property<Glib::ustring>("stun-server", stunUrl.c_str());
         }
//#define GST_TURN_USER "test"   // FIXME - need to read this from the config file
//#define GST_TURN_PASSWORD ""
#ifdef GST_TURN_PASSWORD
         else
         {
            ostringstream u;
            u << "turn://"
              << GST_TURN_USER
              << ":"
              << GST_TURN_PASSWORD
              << "@195.8.117.61";  // FIXME read from config
            Glib::ustring turnUrl = u.str();
            mRtpTransportElement->property<Glib::ustring>("turn-server", turnUrl);
            InfoLog(<<"using TURN config: " << turnUrl); // FIXME security - logs password

         }
#endif
      }
   }
   else
   {
      mRtpTransportElement = mRtpSession->createRtpBinOuter();
   }

   mPipeline->add(mRtpTransportElement);

   // need to create all sink pads on the webrtcbin before it can be used

   // when using webrtcbin we can't go straight into loopback, it looks
   // like we need to create some other source elements and put some caps events
   // and maybe some media through the pipeline before getting the event
   // on-negotiation-needed and completing the webrtcbin setup
   bool loopbackMode = !isWebRTC;

   //RefPtr<Caps> mCapsAudio = Gst::Caps::create_from_string(GST_RTP_CAPS_OPUS);
   if(mCapsAudio)
   {
      RefPtr<Bin> bin = createOutgoingPipeline(mCapsAudio);
      mPipeline->add(bin);
      linkOutgoingPipeline(0, bin, mCapsAudio); // FIXME streamId
      if(loopbackMode)
      {
         sendCapsEvent(bin, mCapsAudio);
      }
      else
      {
         RefPtr<Element> testSrc = Parse::create_bin("audiotestsrc is-live=true wave=red-noise ! audioconvert ! audioresample", true);
         resip_assert(testSrc);
         mPipeline->add(testSrc);
         testSrc->link(bin);
      }
   }

   //RefPtr<Caps> mCapsVideo = Gst::Caps::create_from_string(GST_RTP_CAPS_VP8);
   if(mCapsVideo)
   {
      RefPtr<Bin> bin = createOutgoingPipeline(mCapsVideo);
      mPipeline->add(bin);
      linkOutgoingPipeline(1, bin, mCapsVideo); // FIXME streamId
      if(loopbackMode)
      {
         sendCapsEvent(bin, mCapsVideo);
      }
      else
      {
         RefPtr<Element> testSrc = Parse::create_bin("videotestsrc is-live=true pattern=ball ! videoconvert", true);
         resip_assert(testSrc);
         mPipeline->add(testSrc);
         testSrc->link(bin);
      }
   }

   // FIXME
   RefPtr<Bus> bus = mPipeline->get_bus();
   bus->add_watch(sigc::mem_fun(*this, &GstRemoteParticipant::onGstBusMessage));

   DebugLog(<<"mPipeline and mRtpTransportElement created");

   debugGraph();

   return true;
}

/*void
GstRemoteParticipant::doIceGathering(gstreamer::ContinuationString sdpReady)
{
   std::shared_ptr<gstreamer::WebRtcEndpoint> webRtc = std::static_pointer_cast<gstreamer::WebRtcEndpoint>(mEndpoint);

   std::shared_ptr<gstreamer::EventContinuation> elEventIceCandidateFound =
         std::make_shared<gstreamer::EventContinuation>([this](std::shared_ptr<gstreamer::Event> event){
      DebugLog(<<"received event: " << *event);
      std::shared_ptr<gstreamer::OnIceCandidateFoundEvent> _event =
         std::dynamic_pointer_cast<gstreamer::OnIceCandidateFoundEvent>(event);
      resip_assert(_event.get());

      if(!mTrickleIcePermitted)
      {
         return;
      }
      // FIXME - if we are waiting for a previous INFO to be confirmed,
      //         aggregate the candidates into a vector and send them in bulk
      auto ice = getLocalSdp()->session().makeIceFragment(Data(_event->getCandidate()),
         _event->getLineIndex(), Data(_event->getId()));
      if(ice.get())
      {
         StackLog(<<"about to send " << *ice);
         info(*ice);
      }
      else
      {
         WarningLog(<<"failed to create ICE fragment for mid: " << _event->getId());
      }
   });

   std::shared_ptr<gstreamer::EventContinuation> elIceGatheringDone =
            std::make_shared<gstreamer::EventContinuation>([this, sdpReady](std::shared_ptr<gstreamer::Event> event){
      mIceGatheringDone = true;
      mEndpoint->getLocalSessionDescriptor(sdpReady);
   });

   webRtc->addOnIceCandidateFoundListener(elEventIceCandidateFound, [=](){
      webRtc->addOnIceGatheringDoneListener(elIceGatheringDone, [=](){
         webRtc->gatherCandidates([]{
                  // FIXME - handle the case where it fails
                  // on success, we continue from the IceGatheringDone event handler
         }); // gatherCandidates
      });
   });
}*/

GstRemoteParticipant::StreamKey
GstRemoteParticipant::getKeyForStream(const RefPtr<Caps>& caps) const
{
   if(!caps)
   {
      ErrLog(<<"caps is empty");
      return "";
   }

   DebugLog(<<"getKeyForStream: " << caps->to_string());

   //return pad->get_stream_id();

   string mediaType = caps->get_structure(0).get_name();
   StackLog(<<"mediaType: " << mediaType);
   if(regex_search(mediaType, regex("^video")))
   {
      return "video";
   }
   else if(regex_search(mediaType, regex("^audio")))
   {
      return "audio";
   }
   if(caps->get_structure(0).has_field("media"))
   {
      string mediaName;
      caps->get_structure(0).get_field("media", mediaName);
      StackLog(<<"mediaName: " << mediaName);
      return mediaName.c_str();
   }
   return "";
}

// FIXME - remove this, it is a temporary hack until
// the code is consolidated in GstRtpManager
Data deduceKeyForPadName(const Glib::ustring padName)
{
   StackLog(<<"trying to extract pt from pad name");
   std::regex r("recv_rtp_src_(\\d+)_(\\d+)_(\\d+)");
   std::smatch match;
   const std::string& s = padName.raw();
   if (std::regex_search(s.begin(), s.end(), match, r))
   {
      unsigned int pt = atoi(match[3].str().c_str());
      StackLog(<<"found payload type: " << pt);
      switch(pt)
      {
      case 0: // FIXME
      case 8:
         return "audio";
         break;
      case 97:
         return "video";
         break;
      default:
         resip_assert(0);
         break;
      }
   }

   r = "src_(\\d+)";
   if (std::regex_search(s.begin(), s.end(), match, r))
   {
      unsigned int srcId = atoi(match[1].str().c_str());
      StackLog(<<"found srcId: " << srcId);
      switch(srcId)
      {
      case 0: return "audio";
      case 1: return "video";
      default:
         ErrLog(<<"unsupported srcId value: " << srcId);
         resip_assert(0);
      }
   }

   WarningLog(<<"pad name didn't match regex");
   resip_assert(0);
   return "";
}

bool
GstRemoteParticipant::isWebRTCSession() const
{
   // FIXME - find a better way to do this
   return mRtpTransportElement->get_name() == "webrtcbin0";
}

RefPtr<Pad>
GstRemoteParticipant::createIncomingPipeline(RefPtr<Pad> pad)
{
   DebugLog(<<"createIncomingPipeline: " << pad->get_name());
   auto cDecodePadAdded = [this](const RefPtr<Pad>& pad){
      Glib::ustring padName = pad->get_name();
      DebugLog(<<"DecodeBin: on-pad-added, padName: " << padName << " stream ID: " << pad->get_stream_id());
      if(pad->get_direction() == PAD_SRC)
      {
         RefPtr<Caps> caps = pad->get_current_caps();
         StreamKey key = getKeyForStream(caps);
         if(key.empty())
         {
            key = deduceKeyForPadName(padName);
         }
         resip_assert(key.size());
         auto _enc = mEncodes.find(key);
         if(_enc == mEncodes.end())
         {
            ErrLog(<<"unable to find encoder for " << key);
         }
         else
         {
            bool loopback = !isWebRTCSession();

            RefPtr<EncodeEntry> enc = _enc->second;

            RefPtr<Pad> sink = enc->get_static_pad("sink");
            if(sink->is_linked())
            {
               StackLog(<<"unlinking existing src");
               RefPtr<Pad> src = sink->get_peer();
               if(src->unlink(sink))
               {
                  loopback = true;

                  // FIXME - destroy the unlinked test source

               }
               else
               {
                  ErrLog(<<"failed to unlink existing src");
                  resip_assert(0);
               }
            }

            if(loopback)
            {
               if(key == "video")
               {
                  RefPtr<Gst::VideoConvert> convert = Gst::VideoConvert::create();
                  mPipeline->add(convert);
                  convert->link(enc);
                  pad->link(convert->get_static_pad("sink"));
                  convert->sync_state_with_parent();
               }
               else
               {
                  pad->link(sink);
               }
               enc->sync_state_with_parent();

               // FIXME - here we just loop the incoming to outgoing,
               //         but we should do that in the Conversation class instead
               DebugLog(<<"completed connection from incoming to outgoing stream");
            }
            else
            {
               // FIXME - disconnect the test sources and do the loopback
               ErrLog(<<"not doing loopback for WebRTC");
            }

            debugGraph();

            DebugLog(<<"mDecodes.size() == " << mDecodes.size());
            shared_ptr<flowmanager::HEPRTCPEventLoggingHandler> handler =
                     std::dynamic_pointer_cast<flowmanager::HEPRTCPEventLoggingHandler>(
                     mConversationManager.getMediaStackAdapter().getRTCPEventLoggingHandler());
            if(handler && mDecodes.size() == mEncodes.size())
            {
               DebugLog(<<"all pads ready, trying to setup HOMER HEP RTCP");
               //resip::addGstreamerRtcpMonitoringPads(RefPtr<Bin>::cast_dynamic(mRtpTransportElement));
               // Now all audio and video pads, incoming and outgoing,
               // are present.  We can enable the RTCP signals.  If we
               // ask for these signals before the pads are ready then
               // it looks like we don't receive any signals at all.
               RtcpPeerSpecVector peerSpecs = resip::createRtcpPeerSpecs(*getLocalSdp(), *getRemoteSdp());
               const Data& correlationId = getDialogSet().getDialogSetId().getCallId();
               //addGstreamerRtcpMonitoringPads(RefPtr<Bin>::cast_dynamic(mRtpTransportElement),
               //         handler->getHepAgent(), peerSpecs, correlationId);
            }

         }
      }
   };

   StreamKey streamKey = getKeyForStream(pad->get_current_caps());
   if(streamKey.empty())
   {
      streamKey = deduceKeyForPadName(pad->get_name());
   }

   /*RefPtr<Gst::DecodeBin> decodeBin = Gst::DecodeBin::create();*/

   RefPtr<DecodeEntry> decodeBin = Gst::Bin::create();
   RefPtr<Element> queue = Gst::Queue::create();
   decodeBin->add(queue);
   RefPtr<Element> dec;
   if(streamKey == "video")
   {
      RefPtr<Element> depay;
      RefPtr<Element> parse;
      if(isWebRTCSession())
      {
         depay = Gst::ElementFactory::create_element("rtpvp8depay");
         parse = Gst::ElementFactory::create_element("vp8parse");
         dec = Gst::ElementFactory::create_element("vp8dec");
         //dec->property<guint>("threads", 24); // FIXME magic number
      }
      else
      {
         depay = Gst::ElementFactory::create_element("rtph264depay");
         parse = Gst::ElementFactory::create_element("h264parse");
         dec = Gst::ElementFactory::create_element("openh264dec");
      }
      decodeBin->add(depay);
      decodeBin->add(parse);
      decodeBin->add(dec);
      queue->link(depay);
      depay->link(parse);
      parse->link(dec);
   }
   else if(streamKey == "audio")
   {
      RefPtr<Element> depay;
      if(isWebRTCSession())
      {
         depay = Gst::ElementFactory::create_element("rtpopusdepay");
         dec = Gst::ElementFactory::create_element("opusdec");
      }
      else
      {
         depay = Gst::ElementFactory::create_element("rtppcmadepay");
         dec = Gst::ElementFactory::create_element("alawdec");
      }
      decodeBin->add(depay);
      decodeBin->add(dec);
      queue->link(depay);
      depay->link(dec);
   }
   else
   {
      CritLog(<<"unrecognized streamKey: " << streamKey);
      resip_assert(0);
   }

   RefPtr<Pad> sink = GhostPad::create(queue->get_static_pad("sink"), "sink");
   decodeBin->add_pad(sink);
   //RefPtr<Pad> src = GhostPad::create(dec->get_static_pad("src"), "src");
   RefPtr<Pad> src = GhostPad::create(dec->get_static_pad("src"), pad->get_name());

   mDecodes[streamKey] = decodeBin;
   mPipeline->add(decodeBin);
   decodeBin->signal_pad_added().connect(cDecodePadAdded);

   // Setup the keyframe request probe
   if(streamKey == "video")
   {
      addGstreamerKeyframeProbe(pad, [this](){requestKeyframeFromPeer();});
   }

   decodeBin->sync_state_with_parent();

   // this is where the signal on-pad-added is triggered
   // for any signal handlers created elsewhere
   decodeBin->add_pad(src);

   return sink;
}

RefPtr<Bin>
GstRemoteParticipant::createOutgoingPipeline(const RefPtr<Caps> caps)
{
   DebugLog(<<"creating outgoing bin for caps: " << caps->to_string());
   RefPtr<EncodeEntry> encodeBin = EncodeEntry::create();
   StreamKey streamKey = getKeyForStream(caps);
   resip_assert(streamKey.size());

   RefPtr<Queue> queueIn = Queue::create();
   encodeBin->add(queueIn);

   RefPtr<Pad> queueInSrc = queueIn->get_static_pad("src");

   RefPtr<Caps> _caps, ___caps;

   string mediaType = caps->get_structure(0).get_name();
   StackLog(<<"mediaType: " << mediaType);
   string mediaName;
   if(caps->get_structure(0).has_field("media"))
   {
      caps->get_structure(0).get_field("media", mediaName);
   }
   StackLog(<<"mediaName: " << mediaName);
   string encodingName;
   if(caps->get_structure(0).has_field("encoding-name"))
   {
      caps->get_structure(0).get_field("encoding-name", encodingName);
   }
   StackLog(<<"encodingName: " << encodingName);
   Data payloaderName;
   RefPtr<Element> enc;
   RefPtr<Gst::Element> payloader;
   Glib::RefPtr<Gst::Caps> v_caps;
   if(regex_search(mediaType, regex("^video")) || mediaName == "video")
   {
      GstCaps* __caps = gst_caps_from_string("video/x-raw");
      ___caps = Glib::wrap(__caps);
      _caps = mCapsVideo;
      // profile stuff is only for Gst::EncodeBin
      //encodeBin->property_profile() =
      //GstEncodingVideoProfile* videoProfile =
      //         gst_encoding_video_profile_new(__caps, NULL, NULL, 1);
      //g_object_set(encodeBin->gobj(), "profile", videoProfile, NULL);
      if(encodingName == "VP8")
      {
         // https://gstreamer.freedesktop.org/documentation/vpx/vp8enc.html?gi-language=c
         // https://www.webmproject.org/docs/encoder-parameters/
         // The parameters have slightly different names in gstreamer:
         // https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gst-plugins-good/html/gst-plugins-good-plugins-vp8enc.html
         enc = Gst::ElementFactory::create_element("vp8enc");
         v_caps = Gst::Caps::create_simple("video/x-vp8",
                               "profile", "0");
         enc->property<gint64>("deadline", 0);
         //enc->property<bool>("rt", true);
         //enc->property<guint>("threads", 24); // FIXME magic number
         //enc->property<GstVP8EncTokenPartitions>("token-partitions", 3);  // FIXME magic number - requires 8 cores/threads
         enc->property<gint>("token-partitions", 3);  // FIXME magic number - requires 8 cores/threads
         enc->property<gint>("keyframe-max-dist", 2000);
         // FIXME properties on vp8enc
         payloader = Gst::ElementFactory::create_element("rtpvp8pay");
         payloader->set_property<gint32>("pt", 120); // FIXME magic number
      }
      else if(encodingName == "H264")
      {
         // https://gstreamer.freedesktop.org/documentation/openh264/openh264enc.html?gi-language=c
         enc = Gst::ElementFactory::create_element("openh264enc");
         v_caps = Gst::Caps::create_simple("video/x-h264",
                      "profile", "baseline");
         enc->property<guint>("bitrate", 90000); // low // FIXME magic number
         enc->property<gint64>("complexity", 0); // low
         enc->property<gint64>("rate-control", 1); // bitrate // FIXME magic number

         // https://gstreamer.freedesktop.org/documentation/vaapi/vaapih264enc.html?gi-language=c
         /*enc = Gst::ElementFactory::create_element("vaapih264enc");
         v_caps = Gst::Caps::create_simple("video/x-h264",
                               "profile", "constrained-baseline");
         enc->property<guint>("bitrate", 90); // low // FIXME magic number
         //enc->property<guint>("quality-factor", 26); // ICQ/QVBR bitrate control mode quality
         enc->property<guint>("quality-level", 10); // faster encode, lower quality
         enc->property<gint64>("rate-control", 2); // bitrate // FIXME magic number */

         // https://gstreamer.freedesktop.org/documentation/x264/index.html?gi-language=c
         /*enc = Gst::ElementFactory::create_element("x264enc");
         v_caps = Gst::Caps::create_simple("video/x-h264",
                      "profile", "constrained-baseline");
         enc->property<guint>("bitrate", 90); // low // FIXME magic number
         enc->property<gint64>("pass", 0); // CBR // FIXME magic number
         enc->property<gint64>("tune", 4); // zero latency // FIXME magic number */

         // FIXME properties on enc
         payloader = Gst::ElementFactory::create_element("rtph264pay");
         payloader->set_property<gint32>("pt", 97); // FIXME magic number
      }
      else
      {
         ErrLog(<<"unsupported encoding: " << encodingName);
         resip_assert(0);
      }
   }
   else if(regex_search(mediaType, regex("^audio")) || mediaName == "audio")
   {
      GstCaps* __caps = gst_caps_from_string("audio/x-raw");
      ___caps = Glib::wrap(__caps);
      _caps = mCapsAudio;
      // profile stuff is only for Gst::EncodeBin
      //encodeBin->property_profile() =
      //GstEncodingAudioProfile* audioProfile =
      //         gst_encoding_audio_profile_new(__caps, NULL, NULL, 1);
      //g_object_set(encodeBin->gobj(), "profile", audioProfile, NULL);
      if(encodingName == "OPUS")
      {
         // https://gstreamer.freedesktop.org/documentation/opus/opusenc.html?gi-language=c
         enc = Gst::ElementFactory::create_element("opusenc");
         uint32_t pt = 109; // FIXME magic number
         v_caps = Gst::Caps::create_simple("audio/x-opus",
                                        "payload", pt,
                                        "encoding-name", "OPUS",
                                        "rate", 48000,       // FIXME magic number
                                        "channels", 2);     // FIXME magic number
         enc->property<gint>("bitrate", 48000);
         payloader = Gst::ElementFactory::create_element("rtpopuspay");
         payloader->set_property<gint32>("pt", pt);
      }
      else if(encodingName == "PCMA")
      {
         // https://gstreamer.freedesktop.org/documentation/alaw/alawenc.html?gi-language=c
         enc = Gst::ElementFactory::create_element("alawenc");
         v_caps = Gst::Caps::create_simple("audio/x-alaw",
                                        "rate", 8000,       // FIXME magic number
                                        "channels", 1);     // FIXME magic number
         payloader = Gst::ElementFactory::create_element("rtppcmapay");
         payloader->set_property<gint32>("pt", 8); // FIXME magic number
      }
      else
      {
         CritLog(<<"unsupported encoding: " << encodingName);
         resip_assert(0);
      }
   }
   else
   {
      ErrLog(<<"unhandled mediaType");
      resip_assert(0);
   }

   encodeBin->add(enc);
   encodeBin->add(payloader);
   RefPtr<Gst::Queue> queue = Gst::Queue::create("queueOut");
   encodeBin->add(queue);

   queueInSrc->link(enc->get_static_pad("sink"));

   if(!v_caps)
   {
      enc->link(payloader);
   }
   else
   {
      enc->link(payloader, v_caps);
   }
   payloader->link(queue, _caps);
   RefPtr<Pad> queueSrc = queue->get_static_pad("src");

   RefPtr<Pad> sink = GhostPad::create(queueIn->get_static_pad("sink"), "sink");
   encodeBin->add_pad(sink);
   RefPtr<Pad> src = GhostPad::create(queueSrc, "src");
   encodeBin->add_pad(src);

   // FIXME - move this out of this method
   mEncodes[streamKey] = encodeBin;

   return encodeBin;
}

void
GstRemoteParticipant::debugGraph()
{
   RefPtr<Gst::Bin> bin = mPipeline.cast_dynamic(mPipeline);
   storeGstreamerGraph(bin, "reCon-gst-graph");
}

void
GstRemoteParticipant::createAndConnectElements(bool isWebRTC, std::function<void()> cConnected, CallbackSdpReady sdpReady)
{

   if(isWebRTC)
   {
      createAndConnectElementsWebRTC(cConnected, sdpReady);
   }
   else
   {
      createAndConnectElementsStandard(cConnected, sdpReady);
   }
}

void
GstRemoteParticipant::createAndConnectElementsStandard(std::function<void()> cConnected, CallbackSdpReady sdpReady)
{
   DebugLog(<<"going to STATE_READY");
   mPipeline->set_state(STATE_READY);

   mRtpTransportElement->signal_pad_added().connect([this](const RefPtr<Pad>& pad){
      Glib::ustring padName = pad->get_name();
      DebugLog(<<"WebRTCElement: on-pad-added, padName: " << padName << " stream ID: " << pad->get_stream_id());
      if(pad->get_direction() == PAD_SRC)
      {
         //RefPtr<Caps> rtcpCaps = Gst::Caps::create_from_string("application/x-rtcp");


         // extract ID
         /*std::regex r("recv_rtp_src_(\\d+)_(\\d+)");
         std::smatch match;
         const std::string& s = padName.raw();
         if (std::regex_search(s.begin(), s.end(), match, r))
         {
            unsigned int streamId = atoi(match[1].str().c_str());
            auto& ssrc = match[2];
            StackLog(<<"streamId: " << streamId << " SSRC: " << ssrc);
            RefPtr<Gst::Bin> dec = mDecBin.at(streamId);
            sinkPad = dec->get_static_pad("sink");

            // FIXME - setup HOMER if all pads connected
            //
            // Now all audio and video pads, incoming and outgoing,
            // are present.  We can enable the RTCP signals.  If we
            // ask for these signals before the pads are ready then
            // it looks like we don't receive any signals at all.
            //addGstreamerRtcpMonitoringPads(rtpbin, mHepAgent, mPeerSpecs, mCorrelationId);

            PadLinkReturn ret = newPad->link(sinkPad);

            if (ret != PAD_LINK_OK && ret != PAD_LINK_WAS_LINKED)
            {
               DebugLog(<< "Linking of pads " << padName << " and "
                        << sinkPad->get_name() << " failed.");
            }
            DebugLog(<<"linking done");
            return;
         }*/

         if(!regex_search(padName.raw(), regex("^(in|out)bound")))
         {
            pad->link(createIncomingPipeline(pad));
            debugGraph();
         }
         else
         {
            DebugLog(<<"found RTCP pad: " << pad->get_name());
            // FIXME - for intercepting RTCP flows in Gstreamer rtpbin
            //resip::linkGstreamerRtcpHomer(mPipeline, pad, hepAgent, peerSpacs, correlationId);
            debugGraph();
         }
      }
   });

   //mRtpTransportElement->sync_state_with_parent();

   DebugLog(<<"going to STATE_PLAYING");
   StateChangeReturn ret = mPipeline->set_state(STATE_PLAYING);
   if (ret == STATE_CHANGE_FAILURE)
   {
      ErrLog(<<"pipeline fail");
      mPipeline.reset();
      sdpReady(false, nullptr);
   }

   mRtpSession->onPlaying(); // FIXME - use a signal in GstRtpSession

   //sdpReady(true, mRtpSession->getLocal());
   cConnected();
}

void
GstRemoteParticipant::createAndConnectElementsWebRTC(std::function<void()> cConnected, CallbackSdpReady sdpReady)
{
   // FIXME: store return value, destroy in destructor

   Glib::signal_any<void>(mRtpTransportElement, "on-negotiation-needed").connect([this, cConnected]()
   {
      DebugLog(<<"onNegotiationNeeded invoked");
      // FIXME - do we need to do anything here?
      // The negotiation effort will be initiated by the
      // requests from the SIP peer or local UA.

      // From looking at the sendrecv demo, it appears that this
      // callback is a hint that the webrtcbin element is in
      // a suitable state for the application to call create-offer

      cConnected();
   });

   Glib::signal_any<void,guint,gchararray>(mRtpTransportElement, "on-ice-candidate").connect([this](guint mline_index, gchararray candidate)
   {
      DebugLog(<<"cOnIceCandidate invoked");
      Data mid("0");  // FIXME - why doesn't webrtcbin provide mid?
      onLocalIceCandidate(candidate, mline_index, mid);
   });

   Glib::signal_any<void,GParamSpec*>(mRtpTransportElement, "notify::ice-gathering-state").connect_notify([this, sdpReady](GParamSpec * pspec)
   {
      DebugLog(<<"cIceGatheringStateNotify invoked");
      GstWebRTCICEGatheringState ice_gather_state;
      g_object_get (mRtpTransportElement->gobj(), "ice-gathering-state", &ice_gather_state,
         NULL);
      const Data& newState = lookupGstreamerStateName(iCEGatheringStates, ice_gather_state);
      DebugLog(<<"ICE gathering state: " << newState);

      if(ice_gather_state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)
      {
         GstWebRTCSessionDescription *_gstwebrtcsdp = nullptr;
         g_object_get (mRtpTransportElement->gobj(), "local-description", &_gstwebrtcsdp,
            NULL);
         if(!_gstwebrtcsdp)
         {
            ErrLog(<<"failed to obtain local-description");
            sdpReady(false, nullptr);
         }

         /* Send SDP to peer */
         std::unique_ptr<SdpContents> _sdp(createSdpContentsFromGstreamer(_gstwebrtcsdp));
         gst_webrtc_session_description_free (_gstwebrtcsdp);
         //_sdp->session().transformLocalHold(isHolding());  // FIXME - Gstreamer can hold?

         sdpReady(true, std::move(_sdp));
      }
   });

   DebugLog(<<"going to STATE_READY");
   StateChangeReturn ret = mPipeline->set_state(STATE_READY);
   if (ret == STATE_CHANGE_FAILURE)
   {
      ErrLog(<<"pipeline fail");
      mPipeline.reset();
      sdpReady(false, nullptr);
   }

   Glib::signal_any<void,GParamSpec*>(mRtpTransportElement, "notify::ice-connection-state").connect_notify([this](GParamSpec * pspec)
   {
      GstWebRTCICEConnectionState ice_connection_state;
      g_object_get (mRtpTransportElement->gobj(), "ice-connection-state", &ice_connection_state,
         NULL);
      const Data& newState = lookupGstreamerStateName(iCEConnectionStates, ice_connection_state);
      DebugLog(<<"ICE connection state: " << newState);
      if(ice_connection_state == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED)
      {
         InfoLog(<<"ICE connected");
      }
   });

   // FIXME - commented out while trying to resolve missing on-negotiation-needed
   Glib::signal_any<void,GParamSpec*>(mRtpTransportElement, "notify::signaling-state").connect_notify([this](GParamSpec * pspec)
   {
      ErrLog(<<"signaling-state");
      GstWebRTCSignalingState signaling_state;
      g_object_get (mRtpTransportElement->gobj(), "signaling-state", &signaling_state,
         NULL);
      const Data& newState = lookupGstreamerStateName(signalingStates, signaling_state);
      DebugLog(<<"signaling state: " << newState);
   });

   mRtpTransportElement->signal_pad_added().connect([this](const RefPtr<Pad>& pad){
      DebugLog(<<"WebRTCElement: on-pad-added, stream ID: " << pad->get_stream_id());
      if(pad->get_direction() == PAD_SRC)
      {
         RefPtr<Caps> rtcpCaps = Gst::Caps::create_from_string("application/x-rtcp");
         Glib::ustring padName = pad->get_name();
         if(!regex_search(padName.raw(), regex("^(in|out)bound")))
         {
            pad->link(createIncomingPipeline(pad));
         }
         else
         {
            DebugLog(<<"found RTCP pad: " << pad->get_name());
            // FIXME - for intercepting RTCP flows in Gstreamer rtpbin
            //resip::linkGstreamerRtcpHomer(mPipeline, pad, hepAgent, peerSpacs, correlationId);
            debugGraph();
         }
      }
   });

   //mRtpTransportElement->sync_state_with_parent();

   DebugLog(<<"going to STATE_PLAYING");
   ret = mPipeline->set_state(STATE_PLAYING);
   if (ret == STATE_CHANGE_FAILURE)
   {
      ErrLog(<<"pipeline fail");
      mPipeline.reset();
      sdpReady(false, nullptr);
   }
}

/*void
GstRemoteParticipant::createAndConnectElements(gstreamer::ContinuationVoid cConnected)
{
   // FIXME - implement listeners for some of the events currently using elEventDebug

   std::shared_ptr<gstreamer::EventContinuation> elError =
         std::make_shared<gstreamer::EventContinuation>([this](std::shared_ptr<gstreamer::Event> event){
      ErrLog(<<"Error from Gst MediaObject: " << *event);
   });

   std::shared_ptr<gstreamer::EventContinuation> elEventDebug =
         std::make_shared<gstreamer::EventContinuation>([this](std::shared_ptr<gstreamer::Event> event){
      DebugLog(<<"received event: " << *event);
   });

   std::shared_ptr<gstreamer::EventContinuation> elEventKeyframeRequired =
         std::make_shared<gstreamer::EventContinuation>([this](std::shared_ptr<gstreamer::Event> event){
      DebugLog(<<"received event: " << *event);
      requestKeyframeFromPeer();
   });

   mEndpoint->create([=]{
      mEndpoint->addErrorListener(elError, [=](){
         mEndpoint->addConnectionStateChangedListener(elEventDebug, [=](){
            mEndpoint->addMediaStateChangedListener(elEventDebug, [=](){
               mEndpoint->addMediaTranscodingStateChangeListener(elEventDebug, [=](){
                  mEndpoint->addMediaFlowInStateChangeListener(elEventDebug, [=](){
                     mEndpoint->addMediaFlowOutStateChangeListener(elEventDebug, [=](){
                        mEndpoint->addKeyframeRequiredListener(elEventKeyframeRequired, [=](){
                           //mMultiqueue->create([this, cConnected]{
                           // mMultiqueue->connect([this, cConnected]{
                           mPlayer->create([this, cConnected]{
                              mPassThrough->create([this, cConnected]{
                                 mEndpoint->connect([this, cConnected]{
                                    mPassThrough->connect([this, cConnected]{
                                       //mPlayer->play([this, cConnected]{
                                       cConnected();
                                       //mPlayer->connect(cConnected, *mEndpoint); // connect
                                       //});
                                    }, *mEndpoint);
                                 }, *mPassThrough);
                              });
                           });
                           //}, *mEndpoint); // mEndpoint->connect
                           // }, *mEndpoint); // mMultiqueue->connect
                           //}); // mMultiqueue->create
                        }); // addKeyframeRequiredListener

                     });
                  });
               });
            });
         });
      });
   }); // create
}*/



void
GstRemoteParticipant::setLocalDescription(GstWebRTCSessionDescription* gstwebrtcdesc)
{
   gstWebRTCSetDescription(mRtpTransportElement, "set-local-description", gstwebrtcdesc);
}

void
GstRemoteParticipant::setRemoteDescription(GstWebRTCSessionDescription* gstwebrtcdesc)
{
   gstWebRTCSetDescription(mRtpTransportElement, "set-remote-description", gstwebrtcdesc);
}


void
GstRemoteParticipant::buildSdpOffer(bool holdSdp, CallbackSdpReady sdpReady, bool preferExistingSdp)
{
   bool useExistingSdp = false;
   if(getLocalSdp())
   {
      useExistingSdp = preferExistingSdp || mReuseSdpAnswer;
   }

   try
   {
      bool isWebRTC = true; // FIXME

      // FIXME - option to select audio only
      // FIXME - other codecs (G.711, H.264, ...)
      mCapsVideo = Gst::Caps::create_from_string(GST_RTP_CAPS_VP8);
      mCapsAudio = Gst::Caps::create_from_string(GST_RTP_CAPS_OPUS);

      bool firstUseEndpoint = initEndpointIfRequired(isWebRTC);

      std::function<void(GstPromise * _promise, gpointer user_data)> cOnOfferReady =
               [this, holdSdp, sdpReady](GstPromise * _promise, gpointer user_data){

         DebugLog(<<"cOnOfferReady invoked");
         GstWebRTCSessionDescription *_gstwebrtcoffer = nullptr;

         if(gst_promise_wait(_promise) != GST_PROMISE_RESULT_REPLIED)
         {
            resip_assert(0);
         }

         // FIXME - overriding the const reply with a cast,
         // need to fix in Gstreamermm API
         Gst::Structure reply((GstStructure*)gst_promise_get_reply (_promise));
         gst_structure_get (reply.gobj(), "offer",
            GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &_gstwebrtcoffer, NULL);
         if(!_gstwebrtcoffer)
         {
            Glib::Error __error;
            reply.get_field<Glib::Error>("error", __error);
            ErrLog(<<"failed to get SDP offer: " << __error.what());
            gst_promise_unref (_promise);
            sdpReady(false, nullptr);
            return;
         }
         gst_promise_unref (_promise);

         setLocalDescription(_gstwebrtcoffer);

         /* Send offer to peer */
         std::unique_ptr<SdpContents> _offer(createSdpContentsFromGstreamer(_gstwebrtcoffer));
         gst_webrtc_session_description_free (_gstwebrtcoffer);
         //_offer->session().transformLocalHold(holdSdp);  // FIXME - tell Gstreamer to give us hold SDP

         setLocalSdpGathering(*_offer);

         // FIXME - this should be done now if there is no ICE (non-WebRTC peer)
         //setProposedSdp(*_offer);
         //sdpReady(true, std::move(_offer));
      };

      std::function<void()> cConnected = [this, cOnOfferReady]{
         // FIXME - can we tell Gst to use holdSdp?
         // We currently mangle the SDP after-the-fact in cOnOfferReady

         // From looking at the webrtcbin demo and related discussions in
         // bug trackers, it appears that all the elements should be added to
         // the pipeline and joined together before invoking create-offer.
         // The caps of the sink pads that have already been created and connected
         // on webrtcbin determine which media descriptors (audio, video)
         // will be present in the SDP offer.
         GstPromise *promise;
         std::function<void(GstPromise * _promise, gpointer user_data)> *_cOnOfferReady =
                  new std::function<void(GstPromise * _promise, gpointer user_data)>(cOnOfferReady);
         promise = gst_promise_new_with_change_func (promise_stub, _cOnOfferReady,
                  NULL);
         g_signal_emit_by_name (mRtpTransportElement->gobj(), "create-offer", NULL, promise);

      };

      if(firstUseEndpoint)
      {
         CallbackSdpReady _sdpReady = [this, sdpReady](bool sdpOk, std::unique_ptr<resip::SdpContents> sdp)
         {
            if(!sdpOk)
            {
               sdpReady(false, std::move(sdp));
               return;
            }
            setProposedSdp(*sdp);
            sdpReady(true, std::move(sdp));
         };

         createAndConnectElements(isWebRTC, cConnected, _sdpReady);
      }
      else
      {
         if(!useExistingSdp)
         {
            cConnected();
         }
         else
         {
            std::ostringstream offerMangledBuf;
            getLocalSdp()->session().transformLocalHold(isHolding());
            offerMangledBuf << *getLocalSdp();
            std::shared_ptr<std::string> offerMangledStr = std::make_shared<std::string>(offerMangledBuf.str());
            // FIXME cOnOfferReady(*offerMangledStr);
         }
      }
   }
   catch(exception& e)
   {
      ErrLog(<<"something went wrong: " << e.what());
      sdpReady(false, nullptr);
   }
}



bool
GstRemoteParticipant::onGstBusMessage(const RefPtr<Gst::Bus>&, const RefPtr<Gst::Message>& message)
{
   switch(message->get_message_type())
   {
      case Gst::MESSAGE_EOS:
         ErrLog(<< "End of stream");
         //main_loop->quit();  // FIXME - from echoTest
         return false;
      case Gst::MESSAGE_ERROR:
         {
            Glib::Error error;
            std::string debug_message;
            RefPtr<MessageError>::cast_static(message)->parse(error, debug_message);
            ErrLog(<< "Error: " << error.what() << std::endl << debug_message);
            // main_loop->quit(); // FIXME - from echoTest
            return false;
         }
      case Gst::MESSAGE_STATE_CHANGED:
         DebugLog(<< "state changed");
         return true;
      case Gst::MESSAGE_NEW_CLOCK:
         DebugLog(<< "new clock");
         return true;
      case Gst::MESSAGE_STREAM_STATUS:
         DebugLog(<< "stream status");
         return true;

      default:
         ErrLog(<<"unhandled Bus Message: " << message->get_message_type());
         return true;
   }

   return true;
}

void
GstRemoteParticipant::buildSdpAnswer(const SdpContents& offer, CallbackSdpReady sdpReady)
{
   bool requestSent = false;

   std::shared_ptr<SdpContents> offerMangled(dynamic_cast<SdpContents*>(offer.clone()));
   while(mRemoveExtraMediaDescriptors && offerMangled->session().media().size() > 2)
   {
      // FIXME hack to remove BFCP
      DebugLog(<<"more than 2 media descriptors, removing the last");
      offerMangled->session().media().pop_back();
   }

   try
   {
      // do some checks on the offer
      // check for video, check for WebRTC
      bool isWebRTC = offerMangled->session().isWebRTC();
      DebugLog(<<"peer is " << (isWebRTC ? "WebRTC":"not WebRTC"));

      if(!isWebRTC)
      {
         // RFC 4145 uses the attribute name "setup"
         // We override the attribute name and use the legacy name "direction"
         // from the drafts up to draft-ietf-mmusic-sdp-comedia-05.txt
         // Tested with Gst and Cisco EX90
         // https://datatracker.ietf.org/doc/html/draft-ietf-mmusic-sdp-comedia-05
         // https://datatracker.ietf.org/doc/html/rfc4145

         // FIXME was this only necessary for Kurento?
         //offerMangled->session().transformCOMedia("active", "direction");
      }

      // examine media formats in offer to determine Gstreamer caps
      for(const auto& m : offerMangled->session().media())
      {
         StackLog(<<"parsing a medium entry for caps, name = " << m.name());
         std::list<Data> formats = m.getFormats(); // must have a copy here because it is emptied by m.codecs()
         std::map<int, SdpContents::Session::Codec> codecMap;
         for(const auto& c : m.codecs())
         {
            codecMap[c.payloadType()] = c;
         }
         StackLog(<<"found " << codecMap.size() << " codec(s) and " << formats.size() << " format(s) in the SDP offer");
         resip_assert(formats.size() > 0);
         for(const auto& f : formats)
         {
            StackLog(<<"checking format " << f);
            if(f != "webrtc-datachannel" && f != "*")
            {
               int pt = f.convertInt();
               if(codecMap.find(pt) != codecMap.end())
               {
                  const Codec& c = codecMap.at(pt);
                  Data codecName = c.getName();
                  codecName.lowercase();
                  if(!mCapsAudio && m.name() == "audio")
                  {
                     if(codecName == "opus")
                     {
                        Data caps = "application/x-rtp,media=(string)audio,encoding-name=(string)OPUS,payload=(int)" + Data(pt);
                        mCapsAudio = Gst::Caps::create_from_string(caps.c_str());
                        DebugLog(<<"initialized mCapsAudio for payload type " << pt << ": " << mCapsAudio->to_string());
                     }
                     else if(codecName == "pcma")
                     {
                        Data caps = "application/x-rtp,media=(string)audio,encoding-name=(string)PCMA,clock-rate=(int)8000,payload=(int)" + Data(pt);
                        mCapsAudio = Gst::Caps::create_from_string(caps.c_str());
                        DebugLog(<<"initialized mCapsAudio for payload type " << pt << ": " << mCapsAudio->to_string());
                     }
                  }
                  else if(!mCapsVideo && m.name() == "video")
                  {
                     if(codecName == "vp8")
                     {
                        Data caps = "application/x-rtp,media=(string)video,encoding-name=(string)VP8,payload=(int)" + Data(pt);
                        mCapsVideo = Gst::Caps::create_from_string(caps.c_str());
                        DebugLog(<<"initialized mCapsVideo for payload type " << pt << ": " << mCapsVideo->to_string());
                     }
                     else if(codecName == "h264")
                     {
                        // from EX90 SDP offer:
                        // a=rtpmap:97 H264/90000
                        // a=fmtp:97 packetization-mode=0;profile-level-id=420016;max-br=5000;max-mbps=245000;max-fs=9000;max-smbps=245000;max-fps=6000;max-rcmd-nalu-size=3456000;sar-supported=16
                        Data caps = "application/x-rtp,media=(string)video,encoding-name=(string)H264,clock-rate=(int)90000,payload=(int)" + Data(pt);
                        mCapsVideo = Gst::Caps::create_from_string(caps.c_str());
                        DebugLog(<<"initialized mCapsVideo for payload type " << pt << ": " << mCapsVideo->to_string());
                     }
                  }
               }
               else
               {
                  WarningLog(<<"format " << pt << " not found in codecMap");
               }
            }
         }
      }

      std::shared_ptr<GstWebRTCSessionDescription> gstwebrtcoffer;
      if(isWebRTC)
      {
         gstwebrtcoffer = createGstWebRTCSessionDescriptionFromSdpContents(GST_WEBRTC_SDP_TYPE_OFFER, *offerMangled);
         if(!gstwebrtcoffer)
         {
            ErrLog(<<"failed to convert offer to GstWebRTCSessionDescription");
            sdpReady(false, nullptr);
         }
      }
      else
      {
         // FIXME - IPv6 needed
         // FIXME - MyMessageDecorator will automatically replace 0.0.0.0 with sending IP
         //         but maybe we can do better
         mRtpManager = make_shared<GstRtpManager>("0.0.0.0");
         mRtpSession = make_shared<GstRtpSession>(*mRtpManager);

         std::shared_ptr<SdpContents> answer = mRtpSession->buildAnswer(offerMangled);

         DebugLog(<<"built answer: " << *answer);

         mCapsAudio = mRtpSession->getCaps("audio");
         DebugLog(<<"initialized mCapsAudio: " << mCapsAudio->to_string());
         mCapsVideo = mRtpSession->getCaps("video");
         DebugLog(<<"initialized mCapsVideo: " << mCapsVideo->to_string());
      }

      bool firstUseEndpoint = initEndpointIfRequired(isWebRTC);

      std::function<void(GstPromise* _promise, gpointer user_data)> cAnswerCreated =
               [this, sdpReady](GstPromise * _promise, gpointer user_data)
               {
         DebugLog(<<"cAnswerCreated invoked");

         if(gst_promise_wait(_promise) != GST_PROMISE_RESULT_REPLIED)
         {
            resip_assert(0);
         }

         GstWebRTCSessionDescription *_gstwebrtcanswer = NULL;
         // FIXME - overriding the const reply with a cast,
         // need to fix in Gstreamermm API
         Gst::Structure reply((GstStructure*)gst_promise_get_reply (_promise));
         // FIXME - need Gst::WebRTCSessionDescription
         //_reply.get_field<Gst::WebRTCSessionDescription>("answer",
         //         _gstwebrtcanswer);
         gst_structure_get (reply.gobj(), "answer",
            GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &_gstwebrtcanswer, NULL);
         if(!_gstwebrtcanswer)
         {
            Glib::Error __error;
            reply.get_field<Glib::Error>("error", __error);
            ErrLog(<<"failed to get SDP answer: " << __error.what());
            gst_promise_unref (_promise);
            sdpReady(false, nullptr);
            return;
         }
         gst_promise_unref (_promise);

         setLocalDescription(_gstwebrtcanswer);

         std::unique_ptr<SdpContents> _answer(createSdpContentsFromGstreamer(_gstwebrtcanswer));
         gst_webrtc_session_description_free (_gstwebrtcanswer);
         //_answer->session().transformLocalHold(isHolding());  // FIXME - Gstreamer can hold?
         setLocalSdpGathering(*_answer);

         /* FIXME
                  setLocalSdp(*_answer);
                  setRemoteSdp(*offerMangled);
                  sdpReady(true, std::move(_answer)); */
      };

      std::function<void()> cConnected = [this, isWebRTC, cAnswerCreated, gstwebrtcoffer, offerMangled, sdpReady]() {
         if(isWebRTC)
         {
            // FIXME - Gst::Promise
            DebugLog(<<"submitting SDP offer to Gstreamer element");
            GstPromise *_promise = nullptr;
            /* Set remote description on our pipeline */
            setRemoteDescription(gstwebrtcoffer.get());

            std::function<void(GstPromise * _promise, gpointer user_data)> *_cAnswerCreated =
                     new std::function<void(GstPromise * _promise, gpointer user_data)>(cAnswerCreated);
            _promise = gst_promise_new_with_change_func (promise_stub, _cAnswerCreated,
                     NULL);
            g_signal_emit_by_name (mRtpTransportElement->gobj(), "create-answer", NULL, _promise);
         }
         else
         {
            //cAnswerCreated
            std::unique_ptr<SdpContents> answer(new SdpContents(*mRtpSession->getLocalSdp()));
            setLocalSdp(*answer);
            setRemoteSdp(*offerMangled);
            mGstMediaStackAdapter.runMainLoop();
            sdpReady(true, std::move(answer));
         }
      };



      DebugLog(<<"request SDP answer from Gstreamer");



      CallbackSdpReady _sdpReady = [this, offerMangled, sdpReady](bool sdpOk, std::unique_ptr<resip::SdpContents> sdp)
      {
         if(!sdpOk)
         {
            sdpReady(false, std::move(sdp));
            return;
         }
         setLocalSdp(*sdp);
         setRemoteSdp(*offerMangled);
         sdpReady(true, std::move(sdp));
      };

      if(firstUseEndpoint)
      {
         createAndConnectElements(isWebRTC, cConnected, _sdpReady);
      }
      else
      {
         cConnected();
      }

      /*bool firstUseEndpoint = initEndpointIfRequired(isWebRTC);

      gstreamer::ContinuationString cOnAnswerReady = [this, offerMangled, isWebRTC, sdpReady](const std::string& answer){
         StackLog(<<"answer FROM Gst: " << answer);
         HeaderFieldValue hfv(answer.data(), answer.size());
         Mime type("application", "sdp");
         std::unique_ptr<SdpContents> _answer(new SdpContents(hfv, type));
         _answer->session().transformLocalHold(isHolding());
         setLocalSdp(*_answer);
         setRemoteSdp(*offerMangled);
         sdpReady(true, std::move(_answer));
      };

      gstreamer::ContinuationVoid cConnected = [this, offerMangled, offerMangledStr, isWebRTC, firstUseEndpoint, sdpReady, cOnAnswerReady]{
         if(!firstUseEndpoint && mReuseSdpAnswer)
         {
            // FIXME - Gst should handle hold/resume
            // but it fails with SDP_END_POINT_ALREADY_NEGOTIATED
            // if we call processOffer more than once
            std::ostringstream answerBuf;
            answerBuf << *getLocalSdp();
            std::shared_ptr<std::string> answerStr = std::make_shared<std::string>(answerBuf.str());
            cOnAnswerReady(*answerStr);
            return;
         }
         mEndpoint->processOffer([this, offerMangled, isWebRTC, sdpReady, cOnAnswerReady](const std::string& answer){
            if(isWebRTC)
            {
               if(mTrickleIcePermitted && offerMangled->session().isTrickleIceSupported())
               {
                  HeaderFieldValue hfv(answer.data(), answer.size());
                  Mime type("application", "sdp");
                  std::unique_ptr<SdpContents> _local(new SdpContents(hfv, type));
                  DebugLog(<<"storing incomplete webrtc answer");
                  setLocalSdp(*_local);
                  setRemoteSdp(*offerMangled);
                  ServerInviteSession* sis = dynamic_cast<ServerInviteSession*>(getInviteSessionHandle().get());
                  sis->provideAnswer(*_local);
                  sis->provisional(183, true);
                  //getDialogSet().provideAnswer(std::move(_local), getInviteSessionHandle(), false, true);
                  enableTrickleIce(); // now we are in early media phase, it is safe to send INFO

                  // FIXME - if we sent an SDP answer here,
                  //         make sure we don't call provideAnswer again on 200 OK
               }

               doIceGathering(cOnAnswerReady);
            }
            else
            {
               cOnAnswerReady(answer);
            }
         }, *offerMangledStr); // processOffer
      };

      if(firstUseEndpoint)
      {
         createAndConnectElements(cConnected);
      }
      else
      {
         cConnected();
      }*/

      requestSent = true;
   }
   catch(exception& e)
   {
      ErrLog(<<"something went wrong: " << e.what()); // FIXME - add try/catch to Continuation
      requestSent = false;
   }

   if(!requestSent)
   {
      sdpReady(false, nullptr);
   }
}

void
GstRemoteParticipant::adjustRTPStreams(bool sendingOffer)
{
   // FIXME Gst - implement, may need to break up this method into multiple parts
   StackLog(<<"adjustRTPStreams");

   std::shared_ptr<SdpContents> localSdp = sendingOffer ? getDialogSet().getProposedSdp() : getLocalSdp();
   resip_assert(localSdp);

   std::shared_ptr<SdpContents> remoteSdp = getRemoteSdp();
   bool remoteSdpChanged = remoteSdp.get() != mLastRemoteSdp; // FIXME - better way to do this?
   mLastRemoteSdp = remoteSdp.get();
   if(remoteSdp)
   {
      const SdpContents::Session::Direction& remoteDirection = remoteSdp->session().getDirection();
      if(!remoteDirection.recv())
      {
         setRemoteHold(true);
      }
      else
      {
         setRemoteHold(false);
      }
      if(remoteSdpChanged && mWaitingAnswer)
      {
         // FIXME - maybe this should not be in adjustRTPStreams
         DebugLog(<<"remoteSdp has changed, sending to Gstreamer");
         mWaitingAnswer = false;
         std::shared_ptr<GstWebRTCSessionDescription> gstwebrtcanswer =
                  createGstWebRTCSessionDescriptionFromSdpContents(GST_WEBRTC_SDP_TYPE_ANSWER, *remoteSdp);
         if(!gstwebrtcanswer)
         {
            ErrLog(<<"failed to convert offer to GstWebRTCSessionDescription");
            return;
         }
         setRemoteDescription(gstwebrtcanswer.get());
         /*mEndpoint->processAnswer([this](const std::string updatedOffer){
            // FIXME - use updatedOffer
            WarningLog(<<"Gst has processed the peer's SDP answer");
            StackLog(<<"updatedOffer FROM Gst: " << updatedOffer);
            HeaderFieldValue hfv(updatedOffer.data(), updatedOffer.size());
            Mime type("application", "sdp");
            std::unique_ptr<SdpContents> _updatedOffer(new SdpContents(hfv, type));
            _updatedOffer->session().transformLocalHold(isHolding());
            setLocalSdp(*_updatedOffer);
            //c(true, std::move(_updatedOffer));
         }, answerBuf.str());*/
      }
      for(int i = 1000; i <= 5000; i+=1000)
      {
         std::chrono::milliseconds _i = std::chrono::milliseconds(i);
         std::chrono::milliseconds __i = std::chrono::milliseconds(i + 500);
         mConversationManager.requestKeyframe(mHandle, _i);
         mConversationManager.requestKeyframeFromPeer(mHandle, __i);
      }
   }
}

void
GstRemoteParticipant::addToConversation(Conversation *conversation, unsigned int inputGain, unsigned int outputGain)
{
   RemoteParticipant::addToConversation(conversation, inputGain, outputGain);
}

void
GstRemoteParticipant::removeFromConversation(Conversation *conversation)
{
   RemoteParticipant::removeFromConversation(conversation);
}

bool
GstRemoteParticipant::mediaStackPortAvailable()
{
   return true; // FIXME Gst - can we check with Gst somehow?
}

void
GstRemoteParticipant::waitingMode()
{
/*   std::shared_ptr<gstreamer::MediaElement> e = getWaitingModeElement();
   if(!e)
   {
      return;
   }
   e->connect([this]{
      DebugLog(<<"connected in waiting mode, waiting for peer");
      if(mWaitingModeVideo)
      {
         mPlayer->play([this]{}); // FIXME Gst async
      }
      else
      {
         mEndpoint->connect([this]{
            requestKeyframeFromPeer();
         }, *mPassThrough); // FIXME Gst async
      }
      requestKeyframeFromPeer();
   }, *mEndpoint);*/
}

/*std::shared_ptr<gstreamer::MediaElement>
GstRemoteParticipant::getWaitingModeElement()
{
   if(mWaitingModeVideo)
   {
      return dynamic_pointer_cast<gstreamer::Endpoint>(mPlayer);
   }
   else
   {
      return mPassThrough;
   }
}*/

bool
GstRemoteParticipant::onMediaControlEvent(MediaControlContents::MediaControl& mediaControl)
{
   if(mWSAcceptsKeyframeRequests)
   {
      auto now = std::chrono::steady_clock::now();
      if(now < (mLastLocalKeyframeRequest + getKeyframeRequestInterval()))
      {
         DebugLog(<<"keyframe request ignored, too soon");
         return false;
      }
      mLastLocalKeyframeRequest = now;
      InfoLog(<<"onMediaControlEvent: sending to Gst");
      // FIXME - check the content of the event
      //mEndpoint->sendPictureFastUpdate([this](){}); // FIXME Gst async, do we need to wait for Gst here?
      return true;
   }
   else
   {
      WarningLog(<<"rejecting MediaControlEvent due to config option mWSAcceptsKeyframeRequests");
      return false;
   }
}

void
GstRemoteParticipant::onRemoteIceCandidate(const resip::Data& candidate, unsigned int lineIndex, const resip::Data& mid)
{
   g_signal_emit_by_name (mRtpTransportElement->gobj(), "add-ice-candidate", lineIndex,
             candidate.c_str());
}

bool
GstRemoteParticipant::onTrickleIce(resip::TrickleIceContents& trickleIce)
{
   DebugLog(<<"onTrickleIce: sending to Gst");
   // FIXME - did we already receive a suitable SDP for trickle ICE and send it to Gstreamer?
   //         if not, Gstreamer is not ready for the candidates
   // FIXME - do we need to validate the ice-pwd password attribute here?
   for(auto m = trickleIce.media().cbegin(); m != trickleIce.media().cend(); m++)
   {
      if(m->exists("mid"))
      {
         const Data& mid = m->getValues("mid").front();
         unsigned int mLineIndex = mid.convertInt(); // FIXME - calculate from the full SDP
         if(m->exists("candidate"))
         {
            for(const auto& c : m->getValues("candidate"))
            {
               onRemoteIceCandidate(c, mLineIndex, mid);
            }
         }
      }
      else
      {
         WarningLog(<<"mid is missing for Medium in SDP fragment: " << trickleIce);
         return false;
      }
   }
   return true;
}

void
GstRemoteParticipant::requestKeyframe()
{
   DebugLog(<<"requestKeyframe from local encoder");

   Gst::Structure gSTForceKeyUnit("GstForceKeyUnit",
      "all-headers", true);

   Gst::Iterator<Gst::Pad> it = mRtpTransportElement->iterate_sink_pads();
   while(it.next())
   {
      RefPtr<Pad> pad = *it;
      if(pad)
      {
         StackLog(<<"examining pad " << pad->get_name());
         RefPtr<Pad> peer = pad->get_peer();
         RefPtr<Caps> caps = pad->get_current_caps();
         if(peer && caps)
         {
            if(caps->get_structure(0).has_field("media"))
            {
               string mediaName;
               caps->get_structure(0).get_field("media", mediaName);
               StackLog(<<"mediaName: " << mediaName);
               if(mediaName == "video")
               {
                  RefPtr<Gst::Event> _e = Glib::wrap(gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM,
                           gSTForceKeyUnit.gobj_copy()));
                  peer->send_event(_e);
               }
            }
         }
         else
         {
            DebugLog(<<"no peer or no caps for this pad");
         }
      }
      else
      {
         WarningLog(<<"invalid pad");
      }
   }
}

/* ====================================================================

 Copyright (c) 2022, Software Freedom Institute https://softwarefreedom.institute
 Copyright (c) 2022, Daniel Pocock https://danielpocock.com
 Copyright (c) 2021, SIP Spectrum, Inc.
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
