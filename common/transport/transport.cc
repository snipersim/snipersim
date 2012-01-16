#define TRANSPORT_CC

#include <assert.h>

#include "transport.h"
#include "smtransport.h"
//#include "mpitransport.h"
#include "socktransport.h"

#include "config.h"
#include "log.h"

// -- Transport -- //

Transport *Transport::m_singleton;

Transport::Transport()
{
}

Transport* Transport::create()
{
   // dynamically choose the transport based on number of processes
   // this is required since MPICH seems to break in single-process mode

   assert(m_singleton == NULL);

   if (Config::getSingleton()->getProcessCount() == 1)
      m_singleton = new SmTransport();

   else
      m_singleton = new SockTransport();

   // else if (Config::getSingleton()->getProcessCount() > 1)
   //    m_singleton = new MpiTransport();

   return m_singleton;
}

Transport* Transport::getSingleton()
{
   return m_singleton;
}

// -- Node -- //

Transport::Node::Node(core_id_t core_id)
   : m_core_id(core_id)
{
}

core_id_t Transport::Node::getCoreId()
{
   return m_core_id;
}
