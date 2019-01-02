//
//                           SimuLTE
//
// This file is part of a software released under the license included in file
// "license.pdf". This license can be also found at http://www.ltesimulator.com/
// The above file and the present reference are part of the software itself,
// and cannot be removed from it.
//


#include <math.h>
#include <assert.h>
#include "stack/phy/layer/LtePhyUeMode4D2D.h"
#include "stack/phy/packet/LteFeedbackPkt.h"
#include "stack/d2dModeSelection/D2DModeSelectionBase.h"
#include "stack/mac/packet/LteMode4SchedulingGrant.h"
#include "stack/phy/packet/SpsCandidateResources.h"

Define_Module(LtePhyUeMode4D2D);

LtePhyUeMode4D2D::LtePhyUeMode4D2D()
{
    handoverStarter_ = NULL;
    handoverTrigger_ = NULL;
}

LtePhyUeMode4D2D::~LtePhyUeMode4D2D()
{
}

void LtePhyUeMode4D2D::initialize(int stage)
{
    LtePhyUe::initialize(stage);
    if (stage == 0)
    {
        averageCqiD2D_ = registerSignal("averageCqiD2D");
        d2dTxPower_ = par("d2dTxPower");
        adjacencyPSCCHPSSCH_ = par("adjacencyPSCCHPSSCH");
        pStep_ = par("pStep");
        selectionWindowStartingSubframe_ = par("selectionWindowStartingSubframe");
        numSubchannels_ = par("numSubchannels");
        subchannelSize_ = par("subchannelSize");
        d2dDecodingTimer_ = NULL;
        allocator_ = new LteAllocationModule(this,"D2D");
        transmitting_ = false;
        ThresPSSCHRSRPList_.resize(64);
        // The threshold has a size of 64, and allowable values of 0 - 66
        // Deciding on this for now as it makes the most sense (low priority for both then more likely to take it)
        // High priority for both then less likely to take it.
        std::iota (std::begin(ThresPSSCHRSRPList_), std::end(ThresPSSCHRSRPList_), 1);
    }
}

void LtePhyUeMode4D2D::handleSelfMessage(cMessage *msg)
{
    if (msg->isName("d2dDecodingTimer"))
    {
        while (!sciFrames_.empty()){
            // Get received SCI and it's corresponding RsrpVector
            LteAirFrame* frame = sciFrames_.back();
            std::vector<vector> rsrpVector = sciRsrpVectors_.back();
            std::vector<vector> rssiVector = sciRssiVectors_.back();

            // Remove it from the vector
            sciFrames_.pop_back();
            sciRsrpVectors_.pop_back();
            sciRssiVectors_.pop_back();

            UserControlInfo* lteInfo = check_and_cast<UserControlInfo*>(frame->removeControlInfo());

            // decode the selected frame
            decodeAirFrame(frame, lteInfo, rsrpVector, rssiVector);
        }
        while (!d2dReceivedFrames_.empty())
        {
            LteAirFrame* frame = d2dReceivedFrames_.back();
            std::vector<vector> rsrpVector = tbRsrpVectors_.back();
            std::vector<vector> rssiVector = tbRssiVectors_.back();

            d2dReceivedFrames_.pop_back();
            tbRsrpVectors_.pop_back();
            tbRssiVectors_.pop_back();

            UserControlInfo* lteInfo = check_and_cast<UserControlInfo*>(frame->removeControlInfo());

            // decode the selected frame
            decodeAirFrame(frame, lteInfo, rsrpVector, rssiVector);
        }
        delete msg;
        d2dDecodingTimer_ = NULL;
    }
    else if (msg->isName("createSubframe"))
    {
        createSubframe();
    }
    else
        LtePhyUe::handleSelfMessage(msg);
}

// TODO: ***reorganize*** method
void LtePhyUeMode4D2D::handleAirFrame(cMessage* msg)
{
    UserControlInfo* lteInfo = check_and_cast<UserControlInfo*>(msg->removeControlInfo());

    if (useBattery_)
    {
        //TODO BatteryAccess::drawCurrent(rxAmount_, 0);
    }
    connectedNodeId_ = masterId_;
    LteAirFrame* frame = check_and_cast<LteAirFrame*>(msg);
    EV << "LtePhyUeMode4D2D: received new LteAirFrame with ID " << frame->getId() << " from channel" << endl;
    //Update coordinates of this user
    if (lteInfo->getFrameType() == HANDOVERPKT)
    {
        // check if handover is already in process
        if (handoverTrigger_ != NULL && handoverTrigger_->isScheduled())
        {
            delete lteInfo;
            delete frame;
            return;
        }

        handoverHandler(frame, lteInfo);
        return;
    }

    // Check if the frame is for us ( MacNodeId matches or - if this is a multicast communication - enrolled in multicast group)
    if (lteInfo->getDestId() != nodeId_ && !(binder_->isInMulticastGroup(nodeId_, lteInfo->getMulticastGroupId())))
    {
        EV << "ERROR: Frame is not for us. Delete it." << endl;
        EV << "Packet Type: " << phyFrameTypeToA((LtePhyFrameType)lteInfo->getFrameType()) << endl;
        EV << "Frame MacNodeId: " << lteInfo->getDestId() << endl;
        EV << "Local MacNodeId: " << nodeId_ << endl;
        delete lteInfo;
        delete frame;
        return;
    }

    if (binder_->isInMulticastGroup(nodeId_,lteInfo->getMulticastGroupId()))
    {
        // HACK: if this is a multicast connection, change the destId of the airframe so that upper layers can handle it
        lteInfo->setDestId(nodeId_);
    }

    // send H-ARQ feedback up
    if (lteInfo->getFrameType() == HARQPKT || lteInfo->getFrameType() == GRANTPKT || lteInfo->getFrameType() == RACPKT || lteInfo->getFrameType() == D2DMODESWITCHPKT)
    {
        handleControlMsg(frame, lteInfo);
        return;
    }

    // Deal with and SCI message
    if (lteInfo->getFrameType == SCIPKT)
    {
        // Store the SCI with the appropriate Subchannels (if over multiple store in multiple)
        // When we decode the actual message then we check the associated Subchannels to see if an SCI was successfully received.
        frame->setControlInfo(lteInfo);
        storeAirframe(frame);
    }

    // this is a DATA packet

    // if not already started, auto-send a message to signal the presence of data to be decoded
    if (d2dDecodingTimer_ == NULL)
    {
        d2dDecodingTimer_ = new cMessage("d2dDecodingTimer");
        d2dDecodingTimer_->setSchedulingPriority(10);          // last thing to be performed in this TTI
        scheduleAt(NOW, d2dDecodingTimer_);
    }

    // store frame, together with related control info
    frame->setControlInfo(lteInfo);

    // Capture the Airframe for decoding later
    storeAirFrame(frame);
}

void LtePhyUeMode4D2D::triggerHandover()
{
    // stop active D2D flows (go back to Infrastructure mode)
    // currently, DM is possible only for UEs served by the same cell

    // trigger D2D mode switch
    cModule* enb = getSimulation()->getModule(binder_->getOmnetId(masterId_));
    D2DModeSelectionBase *d2dModeSelection = check_and_cast<D2DModeSelectionBase*>(enb->getSubmodule("lteNic")->getSubmodule("d2dModeSelection"));
    d2dModeSelection->doModeSwitchAtHandover(nodeId_, false);

    LtePhyUe::triggerHandover();
}

void LtePhyUeMode4D2D::doHandover()
{
    // amc calls
    LteAmc *oldAmc = getAmcModule(masterId_);
    LteAmc *newAmc = getAmcModule(candidateMasterId_);
    assert(newAmc != NULL);
    oldAmc->detachUser(nodeId_, D2D);
    newAmc->attachUser(nodeId_, D2D);

    LtePhyUe::doHandover();

    // call mode selection module to check if DM connections are possible
    cModule* enb = getSimulation()->getModule(binder_->getOmnetId(masterId_));
    D2DModeSelectionBase *d2dModeSelection = check_and_cast<D2DModeSelectionBase*>(enb->getSubmodule("lteNic")->getSubmodule("d2dModeSelection"));
    d2dModeSelection->doModeSwitchAtHandover(nodeId_, true);
}

void LtePhyUeMode4D2D::handleUpperMessage(cMessage* msg)
{
//    if (useBattery_) {
//    TODO     BatteryAccess::drawCurrent(txAmount_, 1);
//    }

    UserControlInfo* lteInfo = check_and_cast<UserControlInfo*>(msg->removeControlInfo());

    if (lteInfo->getFrameType() == GRANTPKT)
    {
        // Generate CSRs or save the grant for use when generating SCI information
        LteMode4SchedulingGrant grant = check_and_cast<LteMode4SchedulingGrant*>(msg);
        if (grant->getTotalGrantedBlocks() == 0){
            // Generate a list of CSRs and send it to the MAC layer
            computeCSRs(grant);
        }
        else
        {
            sciGrant_ = grant;
        }
        // No need to go any further.
        return;
    }

    // Store the RBs used for transmission. For interference computation
    RbMap rbMap = lteInfo->getGrantedBlocks();
    UsedRBs info;
    info.time_ = NOW;
    info.rbMap_ = rbMap;

    usedRbs_.push_back(info);

    std::vector<UsedRBs>::iterator it = usedRbs_.begin();
    while (it != usedRbs_.end())  // purge old allocations
    {
        if (it->time_ < NOW - 0.002)
            usedRbs_.erase(it++);
        else
            ++it;
    }
    lastActive_ = NOW;

    if (lteInfo->getFrameType() == DATAPKT && lteInfo->getUserTxParams() != NULL)
    {
        double cqi = lteInfo->getUserTxParams()->readCqiVector()[lteInfo->getCw()];
        if (lteInfo->getDirection() == UL)
            emit(averageCqiUl_, cqi);
        else if (lteInfo->getDirection() == D2D)
            emit(averageCqiD2D_, cqi);

        if (adjacencyPSCCHPSSCH_)
        {
            // Setup so SCI gets 2 RBs from the grantedBlocks.
            RbMap sciRbs;

            RbMap::iterator it;
            std::map<Band, unsigned int>::iterator jt;
            //for each Remote unit used to transmit the packet
            int allocatedBlocks = 0;
            for (it = rbMap.begin(); it != rbMap.end(); ++it)
            {
                if (allocatedBlocks == 2)
                {
                    break;
                }
                //for each logical band used to transmit the packet
                for (jt = it->second.begin(); jt != it->second.end(); ++jt)
                {
                    Band band = jt->first;

                    if (allocatedBlocks == 2)
                    {
                        // Have all the blocks allocated to the SCI so can move on.
                        break;
                    }

                    if (jt->second == 0) // this Rb is not allocated
                        continue;
                    else
                    {
                        // RB is allocated to grant, now give it to the SCI.
                        if (jt->second == 1){
                            jt->second = 0;
                            sciRbs[it->first][band] = 1;
                            ++allocatedBlocks;
                        }
                    }
                }
            }
        }
        else
        {
            // setup so SCI gets 2 RBs from PSCCH allocated RBs
            // TODO: When making a subframe ensure subchannels start at the maximum band.
            // This is based on RIV, we will have the subchannel index, so we need only get the corresponding bands for this message
            // i.e. if subchannel index = 0 sci has band 0,1, index=1 band 2,3, index = 2 band 4,5
            // starting band = subchannelIndex * 2
            Band startingBand = sciGrant_->getStartingSubchannel()*2;
            // Setup so SCI gets 2 RBs from the grantedBlocks.
            RbMap sciRbs;

            RbMap::iterator it;
            std::map<Band, unsigned int>::iterator jt;
            int allocatedRbs = 0;
            //for each Remote unit used to transmit the packet
            for (it = rbMap.begin(); it != rbMap.end(); ++it)
            {
                if (allocatedBlocks == 2)
                {
                    break;
                }
                //for each logical band used to transmit the packet
                for (jt = it->second.begin(); jt != it->second.end(); ++jt)
                {
                    if (allocatedBlocks == 2)
                    {
                        // Have all the blocks allocated to the SCI so can move on.
                        break;
                    }
                    // sciRbs[remote][band] = assigned Rb
                    sciRbs[it->first][jt->first] = 1;
                    ++allocatedRbs;
                }
            }
        }
        UserControlInfo* SCIInfo = lteInfo;
        SCIInfo->setFrameType(SCIPKT);
        SCIInfo->setGrantedBlocks(sciRbs);
    }

    EV << NOW << " LtePhyUeMode4D2D::handleUpperMessage - message from stack" << endl;
    LteAirFrame* frame = NULL;

    if (lteInfo->getFrameType() == HARQPKT)
    {
        frame = new LteAirFrame("harqFeedback-grant");
    }
    else
    {
        // create LteAirFrame and encapsulate the received packet
        SidelinkControlInformation SCI = createSCIMessage(msg);
        sciFrame = prepareAirFrame(SCI, SCIInfo);
        // TODO: Set the MCS for the sciFrame to 0 for sending.

        // TODO: Signal for Sending SCI
        sendBroadcast(sciFrame);

        frame = prepareAirFrame(msg, lteInfo);
    }

    // if this is a multicast/broadcast connection, send the frame to all neighbors in the hearing range
    // otherwise, send unicast to the destination

    EV << "LtePhyUeMode4D2D::handleUpperMessage - " << nodeTypeToA(nodeType_) << " with id " << nodeId_
           << " sending message to the air channel. Dest=" << lteInfo->getDestId() << endl;

    // Mark that we are in the process of transmitting a packet therefor when we go to decode messages we can mark as failure due to half duplex
    transmitting_=true;

    if (lteInfo->getDirection() == D2D_MULTI)
        sendBroadcast(frame);
    else
        sendUnicast(frame);
}

void LtePhyUeMode4D2D::computeCSRs(LteMode4SchedulingGrant* grant)
{
    // Lots to do here

    // Determine the total number of possible CSRs
    int totalPossibleCSRs = (grant->getMaximumLatency() - selectionWindowStartingSubframe_) * numSubchannels_;
    simtime_t startTime = (NOW() + TTI * selectionWindowStartingSubframe_);

    if (grant->getMaximumLatency < 100)
    {
        simtime_t endTime = (NOW() + TTI * grant->getMaximumLatency());
    }
    else
    {
        simtime_t endTime = (NOW() + TTI * 100);
    }

    int subframeIndex = 1000 + selectionWindowStartingSubframe_;
    std::list<std::list<Subchannel*>> selectionWindow;
    for (simtime_t t = startTime; t != endTime ; t+=TTI)
    {
        std::list<Subchannel*> subframe;
        for (int i=0;i<numSubchannels_;i++)
        {
            Subchannel subchannel = new Subchannel(subchannelSize_, t, subframeIndex, i);
            subframe.push_front(subchannel);
        }
        selectionWindow.push_front(subframe);
        ++subframeIndex;
    }

    int thresholdIncreaseFactor = 0;
    std::vector<std::vector<Subchannel>>* possibleCSRs;
    bool checkedSensed = false;

    // Cannot continue until possibleCSRs is at least 20% of all CSRs
    while (possibleCSRs->size() < (totalPossibleCSRs * .2))
    {
        if(!checkedSensed)
        {
            checkSensed(selectionWindow, grant);
            checkedSensed = true;
        }
        checkRSRP(selectionWindow, grant, thresholdIncreaseFactor);
        possibleCSRs = getPossibleCSRs(selectionWindow, grant);
        ++thresholdIncreaseFactor;
    }

    /*
     * Using RSSI pick subchannels with lowest RSSI (Across time) pick 20% lowest.
     * report this to MAC layer.
     */
    std::vector<std::vector<Subchannel>> optimalCSRs;

    if (possibleCSRs->size() > (totalPossibleCSRs * .2))
    {
        optimalCSRs = selectBestRSSIs(possibleCSRs, grant, totalPossibleCSRs);
    }
    else
    {
        optimalCSRs = possibleCSRs;
    }

    // Send the packet up to the MAC layer where it will choose the CSR and the retransmission if that is specified
    // Need to generate the message that is to be sent to the upper layers.
    candidateResourcesMessage = new SpsCandidateResources("CSR Message");
    candidateResourcesMessage->setCSRS(optimalCSRs);
    candidateResourcesMessage->setSchedulingGrant(grant);
    send(candidateResourcesMessage, upperGateOut_);
}

LtePhyUeMode4D2D::checkSensed(std::vector<std::vector<Subchannel*>>* selectionWindow, LteMode4SchedulingGrant* grant)
{
    int pRsvpTx = grant->getPeriod();
    std::vector<unsigned int> allowedRRIs = grant->getPossibleRRIs();
    for (int i=0; i < selectionWindow.size();i++)
    {
        int adjustedIndex = i+1000+selectionWindowStartingSubframe_;
        bool unsensed = false;
        for(int j=0; j<numSubchannels_;j++)
        {
            if (j+grantLength > numSubchannels_)
            {
                // We cannot fill this grant in this subframe and should move to the next one
                break;
            }
            // Check if the current subframe was sensed

            // Check that this frame is allowed to be used i.e. all the previous corresponding frames are sensed.
            pRsvpTxPrime = pStep_ * pRsvpTx / 100;
            int Q = 1;
            // Need to calculate which subframes are not allowed.
            std::vector<unsigned int>::iterator kt;
            for(kt=allowedRRIs.begin(); kt!=allowedRRIs.end(); kt++)
            {
                // This applies to all allowed RRIs as well.
                if (kt < 1 && 10*pStep_ - i < pStep_ * kt)
                {
                    Q = 1/kt;
                }
                for(int z=0; z<sensingWindow_.size(); z++)
                {
                    noUnsensed = true;
                    // Go through through the frames in the sensing window, check if they are sensed
                    if (!sensingWindow[z][0]->getSensed())
                    {
                        int q=1;
                        // If the first subchannel is not sensed then the subframe is not sensed either and as such the subchannels can be removed
                        while (q<Q && noUnsensed)
                        {
                            int resel=0;
                            while (resel<cResel && noUnsensed)
                            {
                                if (adjustedIndex + resel * pRsvpTxPrime == z + pStep_ * kt * q)
                                {
                                    selectionWindow[i][0].setSensed(false);
                                    selectionWindow[i][0].setPossibleCSR(false);
                                    // Allows us to move onto the next subframe and ignore this one as it isn't one that can be selected.
                                    noUnsensed=false;
                                }
                                ++resel;
                            }
                            ++q;
                        }
                    }
                }
            }
        }
    }
}

LtePhyUeMode4D2D::checkRSRP(std::vector<std::vector<Subchannel*>>* selectionWindow, LteMode4SchedulingGrant* grant, int thresholdIncreaseFactor)
{
    std::vector<double> averageRSRPs;
    std::vector<int> priorities;
    std::map<int, std::vector<int>> dissallowedCSRs;
    std::vector<SidelinkControlInformation*> scis;
    bool subchannelReserved = false;
    int grantLength = grant->getNumSubchannels();
    int cResel = grant->getResourceReselectionCounter() * 10;
    int pRsvpTx = grant->getResourceReservationinterval();
    int pRsvpTxPrime = pStep_ * pRsvpTx / 100;

    for (int i=0; i < sensingWindow_.size();i++)
    {
        int subchannelIndex = 0;
        bool unsensed = false;
        if (sensingWindow_[i][0]->getSensed())
        {
            for(int j=0; j<numSubchannels_;j++)
            {
                if (j+grantLength > numSubchannels_)
                {
                    // We cannot fill this grant in this subframe and should move to the next one
                    break;
                }

                for (int k=j; k<j+grantLength; k++)
                {
                    if (sensingWindow_[i][k]->getReserved())
                    {
                        subchannelReserved = true;
                        SidelinkControlInformation* receivedSCI = check_and_cast<SidelinkControlInformation*>(sensingWindow_[i][k]->getSCIMessage());
                        averageRSRPs.push_back(sensingWindow_[i][k]->getAverageRSRP());
                        priorities.push_back(receivedSCI->getPriority());
                        scis.push_back(receivedSCI);

                        int riv = receivedSCI->getFrequencyResourceLocation();
                        std::tuple<int, int> indexAndLength = decodeRivValue(receivedSCI);
                        int lengthInSubchannels = std::get<1>(indexAndLength);
                        k += lengthInSubchannels;
                    }
                }
                if (subchannelReserved)
                {
                    // Get the priorities of both messages
                    int messagePriority = grant->getSpsPriority();

                    for (int l=0; l<averageRSRPs.size(); l++)
                    {
                        double averageRSRP = averageRSRPs[l];
                        int receivedPriority = priorities[l];
                        int receivedSCI = scis[l];
                        // Get the threshold for the corresponding priorities
                        int index = messagePriority * 8 + receivedPriority + 1;
                        int threshold = ThresPSSCHRSRPList_[index];
                        int thresholdDbm = (-128 * (threshold-1)*2) + (3 * thresholdIncreaseFactor);

                        if (averageRSRP > thresholdDbm)
                        {
                            // This series of subchannels is to be excluded
                            // Now how do we calculate this one :P
                            int Q = 1;
                            unsigned int rri = receivedSCI->getResourceReservationInterval();
                            if (rri < 1 && kt <= 1000 - pStep_ * rri)
                            {
                                Q = 1/kt;
                            }

                            for (int q = 1; q <= Q; q++)
                            {
                                for (int c = 0; c < cResel; c++)
                                {
                                    int dissallowedIndex = i + q * pStep * rri -c * pRsvpTxPrime;
                                    for (int k=j; k<j+grantLength; k++){
                                        dissallowedCSRs[dissallowedIndex].push_back(k);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    // Go through the dissallowed indices and set each subchannel if it is dissallowed.
    std::map<int, vector<int>>::iterator it;
    if (grant->getMaximumLatency() < 100)
    {
        maxLatency = grant->getMaximumLatency();
    }
    else
    {
        maxLatency = 100;
    }

    for (it=dissallowedCSRs.begin(); it!=dissallowedCSRs.end(); it++)
    {
        if (it->first >= 1000 + selectionWindowStartingSubframe_ && it->first <= 1000 + maxLatency)
        {
            std::vector<int>::iterator jt;
            for (jt=it->second.begin(); jt!=it->second.end(); ji++)
            {
                selectionWindow[it->first - (1000 + selectionWindowStartingSubframe_)]->setPossibleCSR(false);
            }
        }
    }
}


std::vector<std::vector<Subchannel>>* LtePhyUeMode4D2D::selectBestRSSIs(std::vector<std::vector<Subchannel*>>* possibleCSRs, LteMode4SchedulingGrant* grant, int totalPossibleCSRs)
{
    int decrease = pStep_;
    if (grant->getPeriod() < 100)
    {
        // Same as pPrimeRsvpTx from other parts of the function
        int decrease = (pStep * grant->getPeriod())/100;
    }

    std::vector<std::vector<Subchannel>> optimalCSRs;
    std::vector<std::vector<Subchannel>>::iterator it;

    while(optimalCSRs->size() < (totalPossibleCSRs * 0.2)){
        // I feel like there is definitely a better approach (going through once makes the most sense)
        // But based on the wording of the text this is what they describe.
        // But TODO: Fix this to be more efficient.
        int minRSSI = 0;
        int currentMinCSR = 0;
        for (it=possibleCSRs.begin(); it != possibleCSRs.end(); it++)
        {
            Subchannel* initialSubchannel = it->begin();
            Subchannel* finalSubchannel = it->end();

            int subframe = initialSubchannel->getSubchannelIndex();
            int startingSubChannelIndex = initialSubchannel->getSubchannelIndex();
            int finalSubchannelIndex = finalSubchannel->getSubchannelIndex();

            subframe -= decrease;
            int totalRSSI = 0;
            int numSubchannel = 0;
            while (subframe > 0)
            {
                for (int subchannelCounter = startingSubchannelIndex; subchannelCounter <= finalSubchannelIndex; subchannelCounter++)
                {
                    if (sensingWindow_[subframe][subchannelCounter]->getSensed)
                    {
                        totalRSSI += sensingWindow_[subframe][subchannelCounter]->getAverageRSSI();
                        ++numSubchannels;
                    }
                    else
                    {
                        // Subframe wasn't sensed so skip to the next one.
                        break;
                    }
                }
                subframe -= decrease;
            }
            int averageRSSI = totalRSSI / numSubchannels;
            if (averageRSSI < minRSSI || minRSSI == 0)
            {
                minRSSI = averageRSSI;
                currentMinCSR = it - possibleCSRs.begin();
            }
        }
        optimalCSRs.push_back(possibleCSRs[currentMinCSR]);
        possibleCSRs.erase(currentMinCSR);
        minRSSI = 0;
    }
    return optimalCSRs;
}

std::vector<std::vector<Subchannel>>* LtePhyUeMode4D2D::getPossibleCSRs(std::vector<std::vector<Subchannel*>>* selectionWindow, LteMode4SchedulingGrant* grant)
{
    // Go through the selection window and determine the number of possible CSRs available
    std::vector<std::vector<Subchannel>> possibleCSRs;
    int grantLenght = grant->getNumSubchannels();
    for (int i=0; i < selectionWindow->size(); i++)
    {
        if (selectionWindow[i][0]->getSensed())
        {
            int j=0;
            while (j < numSubchannels_)
            {
                if (j + grantLength > numSubchannels_)
                {
                    // Cannot fit the CSR therefore move on.
                    break;
                }
                std::vector<Subchannel> possibleCSR;
                for (int k=j; k < j+grantLength; k++)
                {
                    if (!selectionWindow[i][k]->getPossibleCSR())
                    {
                        // This range of subchannels will not fit the CSR
                        j = k + 1;
                        break;
                    }
                    possibleCSR.push_back(selectionWindow[i][k]);
                }
                possibleCSRs.push_back(possibleCSR);
                ++j;
            }
        }
    }
    return possibleCSRs;
}

SidelinkControlInformation* LtePhyUeMode4D2D::createSCIMessage(cPacket* message)
{
    SidelinkControlInformation sci = new SidelinkControlInformation("SCI Message");

    /*
     * Priority (based on upper layer)
     * 0-7
     * Mapping unknown, so possibly just take the priority max and min and map to 0-7
     * This needs to be integrated from the application layer.
     * Will take this from the scheduling grant.
     */
    sci->setPriority(sciGrant_->getSpsPriority());

    /* Resource Interval
     *
     * 0 -> 16
     * 0 = not reserved
     * 1 = 100ms (1) RRI [Default]
     * 2 = 200ms (2) RRI
     * ...
     * 10 = 1000ms (10) RRI
     * 11 = 50ms (0.5) RRI
     * 12 = 20ms (0.2) RRI
     * 13 - 15 = Reserved
     *
     * TODO: Make sure the period is set to the correct level (i.e. period = 100/200/300 etc)
     */
    sci->setResourceReservationInterval(sciGrant_->getPeriod()/100);

    /* frequency Resource Location
     * Based on another parameter RIV
     * but size is
     * Log2(Nsubchannel(Nsubchannel+1)/2) (rounded up)
     * 0 - 8 bits
     * 0 - 256 different values
     *
     * Based on TS36.213 14.1.1.4C
     *     if SubchannelLength -1 < numSubchannels/2
     *         RIV = numSubchannels(SubchannelLength-1) + subchannelIndex
     *     else
     *         RIV = numSubchannels(numSubchannels-SubchannelLength+1) + (numSubchannels-1-subchannelIndex)
     */
    unsigned int riv;
    //
    if (sciGrant_->getNumSubchannels() +1 >= numSubchannels_/2)
    {
        // RIV calculation for larger than half+1
        riv = ((numSubchannels_ * (numSubchannels_ - sciGrant_->getNumSubchannels() + 1)) + (numSubchannels_ - sciGrant_->getStartingSubchannel() -1));
    }
    else
    {
        // RIV calculation for less than half+1
        riv = ((numSubchannels_ * (sciGrant_->getNumSubchannels() - 1)) + sciGrant_->getStartingSubchannel());
    }

    sci->setFrequencyResourceLocation(riv);

    /* TimeGapRetrans
     * 1 - 15
     * ms from init to retrans
     */
    sci->setTimeGapRetrans(sciGrant_->getTimeGapTransRetrans());


    /* mcs
     * 5 bits
     * 26 combos
     * TODO: where do we find this guy
     * Technically the MAC layer determines the MCS that it wants the message sent with and as such it will be in the packet
     */
    sci->setMcs(sciGrant_->getMcs());

    /* retransmissionIndex
     * 0 / 1
     * if 0 retrans is in future/this is the first transmission
     * if 1 retrans is in the past/this is the retrans
     */
    if (sciGrant_->getRetransmission())
    {
        sci->setRetransmissionIndex(1);
    }
    else
    {
        sci->setRetransmissionIndex(0);
    }

    /* Filler up to 32 bits
     * Can just set the length to 32 bits and ignore the issue of adding filler
     */
    sci->setBitLength(32);

    return sci;
}

LteAirFrame* LtePhyUeMode4D2D::prepareAirFrame(cPacket* msg, UserControlInfo* lteInfo){
    // Helper function to prepare airframe for sending.
    frame = new LteAirframe("airframe");

    frame->encapsulate(check_and_cast<cPacket*>(msg));
    frame->setSchedulingPriority(airFramePriority_);
    frame->setDuration(TTI);

    lteInfo->setCoord(getRadioPosition());

    lteInfo->setTxPower(txPower_);
    lteInfo->setD2dTxPower(d2dTxPower_);
    frame->setControlInfo(lteInfo);

    return frame;
}

void LtePhyUeMode4D2D::storeAirFrame(LteAirFrame* newFrame)
{
    // implements the capture effect
    // store the frame received from the nearest transmitter
    UserControlInfo* newInfo = check_and_cast<UserControlInfo*>(newFrame->getControlInfo());
    Coord myCoord = getCoord();
    double rsrpMean = 0.0;
    std::vector<double> rsrpVector;
    std::vector<double> rssiVector;

    double sum = 0.0;
    unsigned int allocatedRbs = 0;
    rsrpVector = channelModel_->getRSRP_D2D(newFrame, newInfo, nodeId_, myCoord);
    // TODO: Seems we don't really actually need the enbId, I have set it to 0 as it is referenced but never used for calc
    rssiVector = channelModel_->getSINR_D2D(newFrame, newInfo, nodeId_, myCoord, 0, rsrpVector);

    // TODO: Update the subchannel associated with this transmission to include the average RSRP for the sub channel
    // Need to be able to figure out which subchannel is associated to the Rbs in this case
    if (newInfo->getFrameType() == SCIPKT){
        sciFrames_.push_back(newFrame);
        sciRsrpVectors_.push_back(rsrpVector);
        sciRssiVectors_.push_back(rssiVector);
    }
    else{
        tbFrames_.push_back(newFrame);
        tbRsrpVectors_.push_back(rsrpVector);
        tbRssiVectors_.push_back(rssiVector);
    }
}

void LtePhyUeMode4D2D::decodeAirFrame(LteAirFrame* frame, UserControlInfo* lteInfo, std::vector* rsrpVector, std::vector* rssiVector)
{
    EV << NOW << " LtePhyUeMode4D2D::decodeAirFrame - Start decoding..." << endl;

    if (!transmitting_)
    {
        // apply decider to received packet
        bool result = true;

        RemoteSet r = lteInfo->getUserTxParams()->readAntennaSet();
        if (r.size() > 1)
        {
            // DAS
            for (RemoteSet::iterator it = r.begin(); it != r.end(); it++)
            {
                EV << "LtePhyUeMode4D2D::decodeAirFrame: Receiving Packet from antenna " << (*it) << "\n";

                /*
                 * On UE set the sender position
                 * and tx power to the sender das antenna
                 */

                // cc->updateHostPosition(myHostRef,das_->getAntennaCoord(*it));
                // Set position of sender
                // Move m;
                // m.setStart(das_->getAntennaCoord(*it));
                RemoteUnitPhyData data;
                data.txPower=lteInfo->getTxPower();
                data.m=getRadioPosition();
                frame->addRemoteUnitPhyDataVector(data);
            }
            // apply analog models For DAS
            result=channelModel_->errorDas(frame,lteInfo);
        }
        else
        {
            //RELAY and NORMAL
            if (lteInfo->getDirection() == D2D_MULTI)
                result = channelModel_->error_D2D(frame,lteInfo,rsrpVector_);
            else
                result = channelModel_->error(frame,lteInfo);
        }

        // update statistics
        if (result)
            numAirFrameReceived_++;
        else
            numAirFrameNotReceived_++;

        EV << "Handled LteAirframe with ID " << frame->getId() << " with result "
           << ( result ? "RECEIVED" : "NOT RECEIVED" ) << endl;

        cPacket* pkt = frame->decapsulate();

        // attach the decider result to the packet as control info
        lteInfo->setDeciderResult(result);
        pkt->setControlInfo(lteInfo);

        if(lteInfo->getFrameType() == SCIPKT)
        {
            if (result)
            {
                // TODO: Signal successfully decoded SCI message

                std::tuple<int, int> indexAndLength = decodeRivValue(pkt);
                int subchannelIndex = std::get<0>(indexAndLength);
                int lengthInSubchannels = std::get<1>(indexAndLength);

                bool recordSCIMeasurements = false;
                if (adjacencyPSCCHPSSCH_)
                {
                    recordSCIMeasurements = true;
                }

                std::list<Subchannel>::iterator kt;
                std::list<Subchannel> currentSubframe = sensingWindow_.back();
                for(kt=currentSubframe.begin()+subchannelIndex; kt!=currentSubframe.begin()+subchannelIndex+subchannelLength; kt++)
                {
                    if (recordSCIMeasurements)
                    {
                        // Record RSRP and RSSI for the first two RBs (bands)
                        kt->addRsrpValue(rsrpVector[startingBand], startingBand);
                        kt->addRsrpValue(rsrpVector[startingBand+1], startingBand+1);
                        kt->addRssiValue(rssiVector[startingBand], startingBand);
                        kt->addRssiValue(rssiVector[startingBand+1], startingBand+1);

                        // We only need to record the SCI rsrp and rssi for the first subchannel (the rest will not have any occupied)
                        recordSCIMeasurements = false;
                    }
                    // Record the SCI in the subchannel.
                    kt->setSCI(pkt);
                }
                decodedScis_.push_back(pkt);
            }
            else
            {
                // TODO: Signal failed to decode the SCI message
            }
            // We do not want to send SCIs to the upper layers, as such return now.
            return;
        }
        else
        {
            // Have a TB want to make sure we have the SCI for it.
            bool foundCorrespondingSci = false;
            cPacket* correspondingSCI;
            std::vector<cPacket>::iterator it;
            for(jt=decodedScis_.begin(); jt!=decodedScis_.end(); jt++)
            {
                UserControlInfo* sciInfo = check_and_cast<UserControlInfo*>(jt->removeControlInfo());
                // if the SCI and TB have same source then we have the right SCI
                if (sciInfo->getSourceId() == lteInfo->getSourceId())
                {
                    // Successfully received the SCI
                    foundCorrespondingSci = true;

                    corrspondingSCI = jt;

                    // Remove the SCI
                    decodedScis_.erase(it);
                    break;
                }
            }
            if(result && !foundCorrespondingSci)
            {
                // TODO: Signal failed to decode TB due to lack of sci
                lteInfo->setDeciderResult(false);
                pkt->setControlInfo(lteInfo);
            }
            // TODO: Signal successfully found the SCI message
            else if (!result && foundCorrespondingSci)
            {
                //TODO: Failed to decode TB but decoded the SCI.
            }
            else
            {
                //TODO: Signal successfully decoded both the SCI and TB
            }

            // Now need to find the associated Subchannels, record the RSRP and RSSI for the message and go from there.
            // Need to again do the RIV steps
            std::tuple<int, int> indexAndLength = decodeRivValue(corrspondingSCI);
            int subchannelIndex = std::get<0>(indexAndLength);
            int lengthInSubchannels = std::get<1>(indexAndLength);

            std::list<Subchannel>::iterator kt;
            std::list<Subchannel> currentSubframe = sensingWindow_.back();
            for(kt=currentSubframe.begin()+subchannelIndex; kt!=currentSubframe.begin()+subchannelIndex+subchannelLength; kt++)
            {
                std::vector<Band>::iterator lt;
                std::vector<Band> allocatedBands = kt->getOccupiedBands();
                for (lt=allocatedBands.begin(); lt!=allocatedBands.end(); lt++)
                {
                    // Record RSRP and RSSI for this band
                    kt->addRsrpValue(rsrpVector[lt], lt);
                    kt->addRssiValue(rssiVector[lt], lt);
                }
            }

            // send decapsulated message along with result control info to upperGateOut_
            send(pkt, upperGateOut_);

            if (getEnvir()->isGUI())
                updateDisplayString();
        }
    }
    else
    {
        // update statistics
        numAirFrameNotReceived_++;

        // TODO: Signal to say frame not decoded due to the issue of half duplex antenna

        // Mark subchannels in the current subframe as not having been sensed.
        std::list<Subchannel>::iterator it;
        std::list<Subchannel> currentSubframe = sensingWindow_.back();
        for (it=currentSubframe.begin(); it!=currentSubframe.end(); it++)
        {
            // Mark the subchannel as not having been sensed.
            it->setSensed(false);
        }

        EV << "Handled LteAirframe with ID " << frame->getId() << "Was not decoded due to half-duplex";
    }
}

std::tuple<int,int> LtePhyUeMode4D2D::decodeRivValue(cPacket* sci)
{
    UserControlInfo* lteInfo = sci->getControlInfo();
    RbMap rbMap = lteInfo->getGrantedBlocks();
    RbMap::iterator it;
    std::map<Band, unsigned int>::iterator jt;
    Band startingBand = NULL;

    it = rbMap.begin();

    while (it != rbMap.end() && startingBand == NULL)
    {
        for (jt = it->second.begin(); jt != it->second.end(); ++jt)
       {
           Band band = jt->first;
           if (jt->second == 1) // this Rb is not allocated
           {
               startingBand = band;
               break;
           }
       }
    }

    // Get RIV first as this is common
    SidelinkControlInformation* sci = check_and_cast<SidelinkControlInformation*>(pkt);
    unsigned int RIV = sci->getFrequencyResourceLocation();

    // Get the subchannel Index (allows us later to calculate the length of the message
    int subchannelIndex;
    if (adjacencyPSCCHPSSCH_)
    {
        // If adjacent: Subchannel Index = band//subchannelSize
        subchannelIndex = startingBand+1/subchannelSize_;
    }
    else
    {
        // If non-adjacent: Subchannel Index = band/2
        subchannelIndex = startingBand/2;
    }

    // Based on TS36.213 14.1.1.4C
    // if SubchannelLength -1 < numSubchannels/2
    // RIV = numSubchannels(SubchannelLength-1) + subchannelIndex
    // else
    // RIV = numSubchannels(numSubchannels-SubchannelLength+1) + (numSubchannels-1-subchannelIndex)
    // RIV limit
    // log2(numSubchannels(numSubchannels+1)/2)
    double subchannelLOverHalf = (numSubchannels_ + 2 + (( -1 - subchannelIndex - RIV )/numSubchannels_));
    double subchannelLUnderHalf = (RIV + numSubchannels_ - subchannelIndex)/numSubchannels_;
    int lengthInSubchannels;

    // First the number has to be whole in both cases, it's length + subchannelIndex must be less than the number of subchannels
    if (floor(subchannelLOverHalf) == subchannelLOverHalf && subchannelLOverHalf <= numSubchannels_ && subchannelLOverHalf + subchannelIndex <= numSubchannels_)
    {
        lengthInSubchannels = subchannelLOverHalf;
    }
    // Same as above but also the length must be less than half + 1
    else if (floor(subchannelLUnderHalf) == subchannelLUnderHalf && subchannelLUnderHalf + subchannelIndex <= numSubchannels_ && subchannelLUnderHalf <= numSubchannels /2 + 1)
    {
        lengthInSubchannels = subchannelLUnderHalf;
    }
    return std::make_tuple(subchannelIndex, lengthInSubchannels);
}

void LtePhyUeMode4D2D::createSubframe(simtime_t subframeTime)
{
    // TODO: Figure out where the deployer exists
    // We need it to get the number of Rbs and it must exists somewhere
    allocator_->initAndReset(getDeployer(MacNodeId)->getNumRbUl(), getBinder()->getNumBands());
    std::list<Subchannel> subframe;

    Band band = 0;

    if (!adjacencyPSCCHPSSCH_)
    {
        // This assumes the bands only every have 1 Rb (which is fine as that appears to be the case)
        band = numSubchannels_*2;
    }
    for (int i = 0; i < numSubchannels_; i++)
    {
        Subchannel currentSubchannel = new Subchannel(subchannelSize_, subframeTime);
        // Need to determine the RSRP and RSSI that corresponds to background noise
        // Best off implementing this in the channel model as a method.

        std:vector<Band> occupiedBands;

        int overallCapacity = 0;
        // Ensure the subchannel is allocated the correct number of RBs
        while(overallCapacity < subchannelSize_ && band < getBinder()->getNumBands()){
            // This acts like there are multiple RBs per band which is not allowed.
            overallCapacity += allocator_->getAllocatedBlocks(MAIN_PLANE, MACRO, band);
            occupiedBands.push(band);
            ++band;
        }
        currentSubchannel->setOccupiedBands(occupiedBands);
        subframe.push(currentSubchannel);
    }
    if (sensingWindow_.size() < 10*pStep_)
    {
        sensingWindow_.push_back(subframe);
    }
    else
    {
        sensingWindow_.pop_front();
        sensingWindow_.push_back(subframe);
    }

    // Send self message to trigger another subframes creation and insertion. Need one for every TTI
    createSubframe = new cMessage("createSubframe");
    createSubframe->setSchedulingPriority(0);        // Generate the subframe at start of next TTI
    scheduleAt(NOW + TTI, createSubframe);
}


void LtePhyUeMode4D2D::sendFeedback(LteFeedbackDoubleVector fbDl, LteFeedbackDoubleVector fbUl, FeedbackRequest req)
{
    Enter_Method("SendFeedback");
    EV << "LtePhyUeMode4D2D: feedback from Feedback Generator" << endl;

    //Create a feedback packet
    LteFeedbackPkt* fbPkt = new LteFeedbackPkt();
    //Set the feedback
    fbPkt->setLteFeedbackDoubleVectorDl(fbDl);
    fbPkt->setLteFeedbackDoubleVectorDl(fbUl);
    fbPkt->setSourceNodeId(nodeId_);
    UserControlInfo* uinfo = new UserControlInfo();
    uinfo->setSourceId(nodeId_);
    uinfo->setDestId(masterId_);
    uinfo->setFrameType(FEEDBACKPKT);
    uinfo->setIsCorruptible(false);
    // create LteAirFrame and encapsulate a feedback packet
    LteAirFrame* frame = new LteAirFrame("feedback_pkt");
    frame->encapsulate(check_and_cast<cPacket*>(fbPkt));
    uinfo->feedbackReq = req;
    uinfo->setDirection(UL);
    simtime_t signalLength = TTI;
    uinfo->setTxPower(txPower_);
    uinfo->setD2dTxPower(d2dTxPower_);
    // initialize frame fields

    frame->setSchedulingPriority(airFramePriority_);
    frame->setDuration(signalLength);

    uinfo->setCoord(getRadioPosition());

    frame->setControlInfo(uinfo);
    //TODO access speed data Update channel index
//    if (coherenceTime(move.getSpeed())<(NOW-lastFeedback_)){
//        deployer_->channelIncrease(nodeId_);
//        deployer_->lambdaIncrease(nodeId_,1);
//    }
    lastFeedback_ = NOW;
    EV << "LtePhy: " << nodeTypeToA(nodeType_) << " with id "
       << nodeId_ << " sending feedback to the air channel" << endl;
    sendUnicast(frame);
}

void LtePhyUeMode4D2D::finish()
{
    if (getSimulation()->getSimulationStage() != CTX_FINISH)
    {
        // do this only at deletion of the module during the simulation

        // amc calls
        LteAmc *amc = getAmcModule(masterId_);
        if (amc != NULL)
            amc->detachUser(nodeId_, D2D);

        LtePhyUe::finish();
    }
}
