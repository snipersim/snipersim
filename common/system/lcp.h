#ifndef LCP_H
#define LCP_H

#include "thread.h"
#include "network.h"
#include "message_types.h"
#include "inst_mode.h"

class LCP : public Runnable
{
public:
   LCP();
   ~LCP();

   void run();
   void finish();

   void ackInit(UInt64 count = 0);
   void ackWait();
   void ackSendAck();
   void ackProcessAck();

private:
   void processPacket();

   void updateCommId(void *vp);
   void enablePerformance();
   void disablePerformance();
   void setFrequency(UInt64 core_number, UInt64 freq_in_hz);
   void setInstrumentationMode(InstMode::inst_mode_t inst_mode);
   void handleMagicMessage(LCPMessageMagic *msg);

   SInt32 m_proc_num;
   Transport::Node *m_transport;
   bool m_finished; // FIXME: this should really be part of the thread class somehow

   UInt64 m_ack_num;
};

#endif // LCP_H
