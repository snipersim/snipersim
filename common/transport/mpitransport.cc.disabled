#include <mpi.h>

#include "mpitransport.h"
#include "config.h"

#include "log.h"

// -- MpiTransport -- //

MpiTransport::MpiTransport()
{
   int err_code;
   SInt32 rank;

   SInt32 provided;
   err_code = MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &provided);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Init_thread fail.");
   LOG_ASSERT_ERROR(provided >= MPI_THREAD_MULTIPLE, "Did not get required thread support.");

   err_code = MPI_Comm_rank(MPI_COMM_WORLD, &rank);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Comm_rank fail.");

   Config::getSingleton()->setProcessNum(rank);
   LOG_PRINT("Process number set to %i", Config::getSingleton()->getCurrentProcessNum());

   SInt32 num_procs;
   err_code = MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Comm_size fail.");
   LOG_ASSERT_ERROR(num_procs == (SInt32)Config::getSingleton()->getProcessCount(), "Config no. processes doesn't match MPI no. processes.");

   // FIXME: Although it might seem better to just use PROC_COMM_ID
   // here, doing so fails mysteriously. Using -1 (as in SmTransport)
   // seems to work.
   m_global_node = createNode(-1);
}

MpiTransport::~MpiTransport()
{
   LOG_PRINT("Entering dtor");

   delete m_global_node;

   int err_code;
   err_code = MPI_Finalize();
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Finalize fail.");
}

void MpiTransport::barrier()
{
//    int err_code;
//    err_code = MPI_Barrier(MPI_COMM_WORLD);
//    LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Barrier fail.");

   MPI_Status status;
   SInt32 pkt_size;
   int err_code;
   SInt32 rank;
   SInt32 num_procs;

   err_code = MPI_Comm_rank(MPI_COMM_WORLD, &rank);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Comm_rank fail.");

   err_code = MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Comm_size fail.");

   LOG_PRINT("Entering barrier: %d %d", rank, num_procs);

//   usleep(500);

   if (rank != 0)
   {
      err_code = MPI_Probe(rank - 1, BARR_COMM_TAG, MPI_COMM_WORLD, &status);
      LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Probe fail.");

      err_code = MPI_Get_count(&status, MPI_BYTE, &pkt_size);
      LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Get_count fail.");
      LOG_ASSERT_ERROR(status.MPI_SOURCE != MPI_UNDEFINED, "Message has undefined status?!");
      LOG_ASSERT_ERROR(pkt_size == 1, "Unexpected packet size");

      Byte data;

      err_code = MPI_Recv(&data, 1, MPI_BYTE, rank-1, BARR_COMM_TAG, MPI_COMM_WORLD, &status);
      LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Recv fail.");
      LOG_ASSERT_ERROR(data == 0, "data != 0");

      LOG_PRINT("barrier: %d recv 0", rank);
   }

//   usleep(500);

   {
      Byte data = (rank == num_procs - 1);
      LOG_PRINT("barrier: %d send %d", rank, (int)data);

      err_code = MPI_Send((void*) &data, 1, MPI_BYTE, (rank + 1) % num_procs, BARR_COMM_TAG, MPI_COMM_WORLD);
      LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Send fail.");
   }

//   usleep(500);

   {
      err_code = MPI_Probe((rank + num_procs - 1) % num_procs, BARR_COMM_TAG, MPI_COMM_WORLD, &status);
      LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Probe fail.");

      err_code = MPI_Get_count(&status, MPI_BYTE, &pkt_size);
      LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Get_count fail.");
      LOG_ASSERT_ERROR(status.MPI_SOURCE != MPI_UNDEFINED, "Message has undefined status?!");
      LOG_ASSERT_ERROR(pkt_size == 1, "Unexpected packet size");

      Byte data;

      err_code = MPI_Recv(&data, 1, MPI_BYTE, (rank + num_procs - 1) % num_procs, BARR_COMM_TAG, MPI_COMM_WORLD, &status);
      LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Recv fail.");
      LOG_ASSERT_ERROR(data == 1, "data != 1");

      LOG_PRINT("barrier: %d recv 1", rank);
   }

//   usleep(500);

   if (rank != num_procs - 1)
   {
      Byte data = 1;
      LOG_PRINT("barrier: %d send 1", rank);

      err_code = MPI_Send((void*) &data, 1, MPI_BYTE, (rank + 1) % num_procs, BARR_COMM_TAG, MPI_COMM_WORLD);
      LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Send fail.");
   }

//   usleep(500);

   LOG_PRINT("Exiting barrier");
}

Transport::Node* MpiTransport::getGlobalNode()
{
   return m_global_node;
}

Transport::Node* MpiTransport::createNode(core_id_t core_id)
{
   return new MpiNode(core_id);
}

// -- MpiNode -- //

MpiTransport::MpiNode::MpiNode(core_id_t core_id)
   : Node(core_id)
{
}

MpiTransport::MpiNode::~MpiNode()
{
}

void MpiTransport::MpiNode::globalSend(SInt32 dest_proc, const void *buffer, UInt32 length)
{
   LOG_PRINT("In globalSend");
   send(dest_proc, PROC_COMM_TAG, buffer, length);
}

void MpiTransport::MpiNode::send(SInt32 dest_core, const void *buffer, UInt32 length)
{
   LOG_PRINT("In send");
   int dest_proc = Config::getSingleton()->getProcessNumForCore(dest_core);
   send(dest_proc, dest_core, buffer, length);
}

void MpiTransport::MpiNode::send(SInt32 dest_proc, UInt32 tag, const void *buffer, UInt32 length)
{
   LOG_PRINT("sending msg -- size: %i, tag: %d, dest_proc: %i", length, tag, dest_proc);

   SInt32 err_code;
   err_code = MPI_Send((void*)buffer, length, MPI_BYTE, dest_proc, tag, MPI_COMM_WORLD);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Send fail.");
}

Byte* MpiTransport::MpiNode::recv()
{
   MPI_Status status;
   SInt32 pkt_size, source;
   Byte* buffer;
   int err_code;

   LOG_PRINT("attempting receive -- tag: %i", getCoreId());

   SInt32 tag = getCoreId() == -1 ? PROC_COMM_TAG : getCoreId();

   err_code = MPI_Probe(MPI_ANY_SOURCE, tag, MPI_COMM_WORLD, &status);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Probe fail.");

   err_code = MPI_Get_count(&status, MPI_BYTE, &pkt_size);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Get_count fail.");
   LOG_ASSERT_ERROR(status.MPI_SOURCE != MPI_UNDEFINED, "Message has undefined status?!");
   source = status.MPI_SOURCE;

   LOG_PRINT("msg found -- size: %i, tag: %i, source: %i", pkt_size, tag, source);

   // Allocate a buffer for the incoming message
   buffer = new Byte[pkt_size];

   err_code = MPI_Recv(buffer, pkt_size, MPI_BYTE, source, tag, MPI_COMM_WORLD, &status);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Recv fail.");

   LOG_PRINT("msg recv'd");

   return buffer;
   // NOTE: the caller should free the buffer when it's finished with it
}

bool MpiTransport::MpiNode::query()
{
   SInt32 flag;
   MPI_Status status;
   int err_code;

   SInt32 tag = getCoreId() == -1 ? PROC_COMM_TAG : getCoreId();

   // Probe for a message from any source but with our ID tag
   err_code = MPI_Iprobe(MPI_ANY_SOURCE, tag, MPI_COMM_WORLD, &flag, &status);
   LOG_ASSERT_ERROR(err_code == MPI_SUCCESS, "MPI_Iprobe fail.");

   // flag == 0 indicates that no message is waiting
   return (flag != 0);
}
