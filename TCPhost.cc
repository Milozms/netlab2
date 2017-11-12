#include <click/config.h>
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/vector.cc>
#include <click/args.hh>
#include "TCPhost.hh"
#include "TCPpacket.hh"
#define WINDOW_SIZE 100
#define RECV_BUF_SIZE 100
#define TIME_OUT 1000
CLICK_DECLS
TCPconnection::TCPconnection(uint32_t dst, uint16_t port, TCPhost* host): dstip(dst),tcp_port(port),_seq(1),window_unacked(0, NULL),
window_waiting(0, NULL),receiver_buf(RECV_BUF_SIZE,NULL),state(IDLE),lfs(0),las(0),lar(0), timer(host){}
TCPhost::TCPhost():connections(0, NULL) {}
TCPhost::~TCPhost() {}
TCPconnection* TCPhost::find_connection(uint32_t destip){
    for(Vector<TCPconnection*>::const_iterator i = connections.begin();i!=connections.end();i++){
        if((*i)->dstip==destip){
            return *i;
        }
    }
    return NULL;//connection doesn't exist
}
int TCPhost::configure(Vector<String> &conf, ErrorHandler *errh){
////
    Args(conf, this, errh).read_mp("MY_IP", _my_address).read_mp("DST_IP", _dstip).complete();
    return 0;
}
Packet* TCPhost::write_packet(Packet* p, uint32_t dstip=-1, uint32_t srcip=-1, uint32_t seqnum=-1, uint32_t acknum=-1,
                                      bool synflag=false, bool ackflag=false, bool finflag=false){
    WritablePacket* packet = p->clone()->uniqueify();
    struct TCPheader* header = (struct TCPheader*) packet->data();
    if(dstip != -1) header->dstip = dstip;
    if(srcip != -1) header->srcip = srcip;
    if(seqnum != -1) header->seqnum = seqnum;
    if(acknum != -1) header->acknum = acknum;
    return packet;
}

Packet* TCPhost::make_packet(uint32_t dstip, uint32_t srcip, uint32_t seqnum, uint32_t acknum,
                                      bool synflag=false, bool ackflag=false, bool finflag=false){
    WritablePacket *packet = Packet::make(0,0,sizeof(struct TCPheader), 0);
    memset(packet->data(),0,packet->length());
    struct TCPheader* header = (struct TCPheader*) packet->data();
    header->dstip = dstip;
    header->srcip = srcip;
    header->seqnum = seqnum;
    header->acknum = acknum;
    header->SYN_TCP = synflag;
    header->ACK_TCP = ackflag;
    header->FIN = finflag;
    return packet;
}
void TCPhost::run_timer(Timer* timer){
    click_chatter("Timer.");
    for(Vector<TCPconnection*>::const_iterator i = connections.begin();i!=connections.end();i++){
        TCPconnection* conn = *i;
        if(timer == &(conn->timer)){
            if(conn->state == ACTIVE_PENDING){
                //Packet* syn = write_packet(conn->window_unacked[0]);//Don't!!!
                Packet *syn = make_packet(conn->dstip, _my_address, conn->_seq, 0, true);
                struct TCPheader* syn_header = (struct TCPheader*) syn->data();
                click_chatter("Sending SYN(SEQ = %u) to %u.", syn_header->seqnum, syn_header->dstip);
                output(1).push(syn);
                timer->schedule_after_msec(TIME_OUT);
            }
            else if (conn->state == PASSIVE_PENDING){
                Packet* syn = write_packet(conn->window_unacked[0]);
                struct TCPheader* syn_header = (struct TCPheader*) syn->data();
                click_chatter("Sending SYN ACK(SEQ = %u, ACK = %u) to %u.", syn_header->seqnum, syn_header->acknum, syn_header->dstip);
                output(1).push(syn);
                timer->schedule_after_msec(TIME_OUT);
            }
            else{
                Packet* pkt = write_packet(conn->window_unacked[0]);
                struct TCPheader* header = (struct TCPheader*) pkt->data();
                click_chatter("Sending DATA to %u, seq = %u.", header->dstip, header->seqnum);
                output(1).push(pkt);
                timer->schedule_after_msec(TIME_OUT);
            }
        }
    }
    click_chatter("Timer end.");
}
//input port 0: from ip
//output port 0: to ip layer
void TCPhost::push(int port, Packet* packet)
{
    assert(packet);
    if(port == 0){//data from upper layer
        //tmp: set dest ip
        packet = write_packet(packet, _dstip);
        struct TCPheader* header = (struct TCPheader*) packet->data();
        TCPconnection* conn;
        conn = find_connection(_dstip);
        if(conn==NULL){//not exist --> send syn
            //connection establish
            conn = new TCPconnection(_dstip, connections.size(), this);
            click_chatter("Note: new connection to ip %u.", _dstip);
            connections.push_back(conn);
            Packet* syn = make_packet(header->dstip, _my_address, conn->_seq, 0, true);
//            Packet* syn = make_packet(conn->dstip, _my_address, conn->_seq, 0, true);
            struct TCPheader* syn_header = (struct TCPheader*) syn->data();
            //conn->_seq++;

            conn->timer.initialize(this);
            conn->timer.schedule_after_msec(TIME_OUT);
            conn->state = ACTIVE_PENDING;
            //conn->window_unacked.push_back(syn->clone()); // Don't!!!
            conn->window_waiting.push_back(packet->clone());
            click_chatter("Sending SYN(SEQ = %u) to %u.", syn_header->seqnum, syn_header->dstip);
            output(1).push(syn);
            //timer
        }
        else if(conn->state != ESTABLISHED){
            click_chatter("Connection unestablished, Packet Waiting.");
            conn->window_waiting.push_back(packet->clone());
        }
        else{
            if(conn->window_unacked.size()<WINDOW_SIZE){//window not full
                Packet* new_packet = write_packet(packet, -1, _my_address, conn->_seq, -1);
                TCPheader* new_packet_header = (struct TCPheader*) new_packet->data();
                conn->window_unacked.push_back(new_packet->clone());
                conn->_seq++;
                click_chatter("Sending DATA to %u, seq = %u.", new_packet_header->dstip, new_packet_header->seqnum);
                output(1).push(new_packet); //to ip layer
                conn->lfs++;
            }
            else{//window full
                click_chatter("Window full, Packet Waiting.");
                conn->window_waiting.push_back(packet->clone());
            }
        }        

    }
    else if(port == 1){//data from ip layer
        struct TCPheader* header = (struct TCPheader*) packet->data();
        TCPconnection* conn;
        conn = find_connection(header->srcip);
        click_chatter("Received packet: dstip %d, srcip %d, myip %d",header->dstip,header->srcip,_my_address);
        click_chatter("Received packet %u from %u %u", header->seqnum, header->srcip, header->acknum);
        if(conn == NULL){
            click_chatter("Error: connection to ip %u not found.", header->srcip);
        }
        if(header->SYN_TCP && !header->ACK_TCP){
            click_chatter("Branch 1: receive SYN, packet->seq = %u.", header->seqnum);
            //received syn --> send syn ack
            if(conn==NULL){//not exist
                //connection establish
                conn = new TCPconnection(header->srcip, connections.size(), this);
                connections.push_back(conn);
                Packet *syn = make_packet(header->srcip, _my_address, conn->_seq, header->seqnum+1, true, true);
                struct TCPheader* syn_header = (struct TCPheader*) syn->data();
                //syn_heaser->dstport

                click_chatter("Sending SYN ACK(SEQ = %u, ACK = %u) to %u.", syn_header->seqnum, syn_header->acknum, syn_header->dstip);
                //conn->las++;
                conn->window_unacked.push_back(syn->clone());
                output(1).push(syn);
                conn->timer.initialize(this);
                conn->timer.schedule_after_msec(TIME_OUT);
                conn->state = PASSIVE_PENDING;
                //timer
            }
        }
        else if(header->SYN_TCP && header->ACK_TCP){
            click_chatter("Branch 2: receive SYN ACK, packet->seq = %u.", header->seqnum);
            //received syn ack
            if(header->acknum == conn->_seq + 1){
                click_chatter("Receive SYN ACK, Connection to %u Established.", header->srcip);
                conn->state = ESTABLISHED;
                //send ack back
                Packet *ack = make_packet(header->srcip, _my_address, header->acknum, header->seqnum+1, false, true);
                struct TCPheader* ack_header = (struct TCPheader*) ack->data();
                click_chatter("Sending ACK_TCP(SEQ = %u, ACK = %u) to %u.", ack_header->seqnum, ack_header->acknum, ack_header->dstip);
                output(1).push(ack);
                //conn->window_unacked.pop_front(); //Don't!!!
                conn->timer.unschedule();
                //begin sending
                click_chatter("Number of waiting packets: %u.", conn->window_waiting.size());
                while(conn->window_waiting.size()>0){
                    click_chatter("Pop from queue.");
                    Packet* poppacket = write_packet(conn->window_waiting[0], -1, -1, conn->_seq);
                    conn->window_waiting.pop_front();
                    //send it
                    struct TCPheader* header = (struct TCPheader*) poppacket->data();
                    conn->window_unacked.push_back(packet->clone());
                    conn->_seq++;
                    click_chatter("Sending DATA to %u, seq = %u.", header->dstip, header->seqnum);
                    output(1).push(poppacket); //to ip layer
                    conn->lfs++;

                }
            }

        }
        else if(header->ACK_TCP){
            //received ack
            if(conn==NULL){
                //not exist
                click_chatter("Error: Connection not exist.");
            }
            else{
                if(conn->state == PASSIVE_PENDING){
                    if(header->acknum == conn->_seq+1){
                        //received ack for connection establish
                        click_chatter("Receive ACK_TCP, Connection to %u Established.", header->srcip);
                        conn->state = ESTABLISHED;
                        conn->window_unacked.pop_front();
                        conn->timer.unschedule();
                    }
                }
                else if(conn->state == ESTABLISHED){
                    click_chatter("Receive ACK_TCP, LAR = %u.", conn->lar);
                    conn->lar++;
                    //update sliding window
                    conn->window_unacked.pop_front();
                    conn->timer.unschedule();
                    click_chatter("Number of waiting packets: %u.", conn->window_waiting.size());
                    while(conn->window_waiting.size()>0){
                        click_chatter("Pop from queue.");
                        Packet* poppacket = write_packet(conn->window_waiting[0], -1, -1, conn->_seq);
                        conn->window_waiting.pop_front();
                        //send it
                        struct TCPheader* header = (struct TCPheader*) poppacket->data();
                        conn->window_unacked.push_back(packet->clone());
                        conn->_seq++;
                        click_chatter("Sending DATA to %u, seq = %u.", header->dstip, header->seqnum);
                        output(1).push(poppacket); //to ip layer
                        conn->lfs++;

                    }
                }
            }
            

        }
        else{//receive data -> send ack （to ip layer)
            //buffer las+1~las+W
            if(conn!=NULL){
                click_chatter("Receiving DATA, seq = %u.", header->seqnum);
                if(header->seqnum > conn->las + RECV_BUF_SIZE || header->seqnum <= conn->las){
                    click_chatter("Buffer overflow, discard packet.");
                }
                else{
                    conn->receiver_buf[header->seqnum - conn->las - 1] = packet->clone();
                    click_chatter("Buffer packet at position %u, seq = %u.", header->seqnum - conn->las - 1, header->seqnum);
                    while(conn->receiver_buf[0] != NULL){
                        //send ack for las
                        struct TCPheader* header = (struct TCPheader*) conn->receiver_buf[0]->data();
                        Packet* ack = make_packet(header->srcip, _my_address, header->seqnum, 0, false, true);
                        struct TCPheader* ack_header = (struct TCPheader*) ack->data();
                        click_chatter("Sending ACK_TCP for seq = %u to %u.", ack_header->seqnum, ack_header->dstip);
                        output(1).push(ack);
                        conn->las++;
                        conn->receiver_buf.pop_front();
                        conn->receiver_buf.push_back(NULL);
                    }
                }
            }
        }
        
        
    }
    packet->kill();
}

CLICK_ENDDECLS
EXPORT_ELEMENT(TCPhost)