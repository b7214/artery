/*
 * CSMA_LPL
 *
 * A very simple mac protocol. If we want to send a packet,
 * we sense if the medium is free (using rssi). If it is free,
 * send the packet. If the medium is not free, we delay for some
 * time and try again. We also delay if we just sent a msg.
 *
 * Receiving is done by using polling to detect the preamble.
 * When a preamble is detected the radio stays on until a packet
 * is received.
 */

#include "mixim.h"
#include "message.h"

#include "csmaack.h"

Define_Module_Like( CsmaAck, MacClass );

void CsmaAck::initialize() {
	Mac::initialize();
	
	proto_state = PROTO_STATE_IDLE;
	proto_next_state = PROTO_STATE_NONE;
	tx_msg = NULL;
	nav_state = NAV_STATE_CLEAR;
	nav_end_time = 0;
	data_contend_time = DATA_CONTEND_TIME;

	max_packet_retries = getLongParameter("maxPacketRetries", PACKET_RETRIES);

	// start listening
	setRadioListen();
}

CsmaAck::~CsmaAck()
{
	if (tx_msg)
		delete tx_msg;
}

void CsmaAck::txPacket(Packet * msg) {
	assert(msg);
	assert(msg->local_to != node->getNodeId());
	if(tx_msg) {
		printf(PRINT_MAC, "got message while busy");
		++stat_tx_drop;
		delete msg;
		return;
	}
	tx_msg = msg;
	evalState();
	packet_retries = max_packet_retries;
}

void CsmaAck::setIdle() {
	proto_state = PROTO_STATE_IDLE;
	evalState();
}

void CsmaAck::evalState() {
	if(proto_state == PROTO_STATE_IDLE && !isReceiving()) {
		// idling
		if(nav_state == NAV_STATE_CLEAR) {
			// listening / active state
			if(tx_msg) {
				printf(PRINT_MAC, 
					"preparing to send data -> %d",
					tx_msg->local_to);
				proto_next_state = PROTO_STATE_SEND_DATA;
				startContending(DATA_CONTEND_TIME);
				return;
			}
			// nothing to do, listen
			printf(PRINT_MAC, "idle listening");
			setRadioListen();
		} else {
			// sleep state
			printf(PRINT_MAC, "idle sleeping");
			setRadioSleep();
		}
	}
}

void CsmaAck::startContending(int time) {
	assert(proto_next_state >= 0); // must have something todo
	assert(time >= 5);
	if(nav_state == NAV_STATE_BUSY) {
		printf(PRINT_MAC, "contend: skipping because nav is busy");
		proto_next_state = PROTO_STATE_NONE;
		setIdle();
	} else {
		proto_state = PROTO_STATE_CONTEND;
		int ctime = (int)intuniform(5, time);
		printf(PRINT_MAC, "starting contention, will fire in %d", ctime);
		setRadioListen(); 
		setProtocolTimeout(ctime);
	}
}

void CsmaAck::rxFrame(Packet * msg){
	assert(msg);
	Header *header = (Header*) msg->getData(MAC_DATA);
	if(proto_state == PROTO_STATE_WFACK && 
			(header->kind != KIND_ACK 
			 || msg->local_to != node->getNodeId()))
	{
		printf(PRINT_MAC, "received packet, but not ack we want");
		incBackoff();
		cancelTimeout(TIMER_PROTOCOL);
		proto_state = PROTO_STATE_IDLE;
	}
			
	switch(header->kind) {
		case KIND_ACK:
			receiveAck(msg);
			break;
		case KIND_DATA:
			receiveData(msg);
			break;
		default:
			assert(false); // unknown msg
	}
	evalState();
}

void CsmaAck::transmitDone(){
	printf(PRINT_MAC, "transmitDone");
	switch(proto_state) {
		case PROTO_STATE_SEND_ACK:
			setIdle();
			break;
		case PROTO_STATE_SEND_DATA:
			proto_state = proto_next_state;
			if (proto_state == PROTO_STATE_WFACK)
				setProtocolTimeout(TIMEOUT_WFACK);
			assert(tx_msg);
			if (tx_msg->local_to == BROADCAST) {
				++stat_tx;
				tx_msg->setKind(TX_DONE);
				txPacketDone(tx_msg);
				tx_msg = NULL;
			}
			setRadioListen();
			break;
		default:
			assert(false); // unknown
	}
}

void CsmaAck::rxFailed() {
	evalState();
}

void CsmaAck::rxStarted() {
	// if we were contending, cancel it
	if(proto_state == PROTO_STATE_CONTEND) {
		printf(PRINT_MAC, "reception started, cancelling contention");
		cancelTimeout(TIMER_PROTOCOL);
		proto_state = PROTO_STATE_IDLE;
		proto_next_state = PROTO_STATE_NONE;
	}
}

void CsmaAck::protocolTimeout() {
	ProtoState next_state;
	switch(proto_state) {
		case PROTO_STATE_CONTEND:
			assert(proto_next_state >= 0);
			assert(!isReceiving()); // should be cancelled
			assert(nav_state == NAV_STATE_CLEAR);
			// take a rssi sample, to be sure
			setRadioListen(); // make sure the sample is taken now (LPL)
			if(getRssi()>0.5) { // someone in the air, restart 
				printf(PRINT_MAC, 
			"sensed communication, cancelling");
				setIdle();
				return;
			}
			// start the next state
			next_state = proto_next_state;
			proto_next_state = PROTO_STATE_NONE;
			switch(next_state) {
				case PROTO_STATE_SEND_ACK:
					sendAck();
					break;
				case PROTO_STATE_SEND_DATA:
					sendData();
					break;
				default: assert(false); // invalid
			}
			break;
		case PROTO_STATE_WFACK:
			printf(PRINT_MAC, "wait-for-ack timeout <- %d", tx_msg->local_to);
			if (packet_retries-- == 0) {
				++stat_tx_drop;
				tx_msg->setKind(TX_FAILED);
				txPacketDone(tx_msg);
				tx_msg = NULL;
			} else {
				incBackoff();
				setIdle();	// retry
			}
			break;
			
		default:
			assert(false); // invalid
	}
}

void CsmaAck::sendAck() {
	Header header;
	printf(PRINT_MAC, "sending ack -> %d", ack_to);
	proto_state = PROTO_STATE_SEND_ACK;
	Packet * msg = new Packet("ACK");
	msg->local_from = node->getNodeId();
	assert(ack_to >=0);
	msg->local_to = ack_to;
	header.kind = KIND_ACK;
	msg->setData(MAC_DATA, &header, sizeof(header), 0);
	msg->setLength(0);
	setRadioTransmit();
	reg_tx_overhead(msg);
	startTransmit(msg);
}

void CsmaAck::sendData() {
	Header header;
	printf(PRINT_MAC, "sending data -> %d", tx_msg->local_to);
	proto_state = PROTO_STATE_SEND_DATA;
	assert(tx_msg);
	assert(tx_msg->local_to != node->getNodeId());
	Packet *msg = (Packet *)tx_msg->dup();
	msg->local_from = node->getNodeId();
	header.kind = KIND_DATA;
	msg->setData(MAC_DATA, &header, sizeof(header), 0);
	if (msg->local_to == BROADCAST)
		proto_next_state = PROTO_STATE_IDLE;
	else
		proto_next_state = PROTO_STATE_WFACK;
	setRadioTransmit();
	reg_tx_data(msg);
	startTransmit(msg);
}

void CsmaAck::receiveAck(Packet * msg) {
	assert(msg->local_to != -1);
	if(msg->local_to == node->getNodeId()) {
		reg_rx_overhead(msg);
		if(proto_state != PROTO_STATE_WFACK
				|| msg->local_from != tx_msg->local_to)
		{
			printf(PRINT_MAC, "ignoring unsoll. ack");
		} else {
			cancelTimeout(TIMER_PROTOCOL);
			decBackoff();
			printf(PRINT_MAC, "received ack <- %d",
					msg->local_from);
			// cleanup
			assert(tx_msg);
			++stat_tx;
			tx_msg->setKind(TX_DONE);
			txPacketDone(tx_msg);
			tx_msg = NULL;
			setIdle();
		}
	} else {
		printf(PRINT_MAC, "received ack for %d (not me)",
				msg->local_to);
		reg_rx_overhear(msg);
	}
	delete msg;
}

void CsmaAck::receiveData(Packet * msg) {
	if(msg->local_to == node->getNodeId()) {
		ack_to = msg->local_from;

		// check if we already heard this one
		printf(PRINT_MAC, "received unicast packet <- %d", msg->local_from);
		reg_rx_data(msg);
		rxPacket(msg);
		++stat_rx;

		proto_next_state = PROTO_STATE_SEND_ACK;
		startContending(ACK_CONTEND_TIME);
	} else if(msg->local_to == BROADCAST) {
		proto_state = proto_next_state = PROTO_STATE_IDLE;
		printf(PRINT_MAC, "received broadcast packet <- %d", 
				msg->local_from);
		reg_rx_data(msg);
		rxPacket(msg);
		++stat_rx;
	} else {
		proto_state = proto_next_state = PROTO_STATE_IDLE;
		printf(PRINT_MAC, "overheard data packet");
		reg_rx_overhear(msg);
		// give time to send ack
		updateNav(NAV_ACK);
		delete msg;
	}
}

void CsmaAck::updateNav(ushort t) {
	assert(t>0);
	ushort now = node->getCurrentTime();
	ushort nav_left = nav_end_time - now;
	if(nav_state ==  NAV_STATE_CLEAR || t > nav_left) {
		printf(PRINT_MAC, "updating NAV, left = %u", (unsigned)t);
		setNavTimeout(t);
		nav_state = NAV_STATE_BUSY;
		nav_end_time = t + now;
	}
}

void CsmaAck::navTimeout(){
	printf(PRINT_MAC, "NAV timer, medium clear now");
	nav_state = NAV_STATE_CLEAR;
	evalState();
}

int CsmaAck::headerLength(){ return 7; }

void CsmaAck::timeout(int which){
	switch(which) {
		case TIMER_PROTOCOL:
			protocolTimeout();
			break;
		case TIMER_NAV:
			navTimeout();
			break;
		default:
			assert(false); // unknown timer
	}
}

void CsmaAck::setProtocolTimeout(int t) {
	setTimeout(t, TIMER_PROTOCOL);
}

void CsmaAck::setNavTimeout(int t) {
	setTimeout(t, TIMER_NAV);
}

void CsmaAck::incBackoff() {
	data_contend_time *= 2;
	if(data_contend_time > MAX_DATA_CONTEND_TIME)
		data_contend_time = MAX_DATA_CONTEND_TIME;
}

void CsmaAck::decBackoff() {
	data_contend_time = DATA_CONTEND_TIME;
}