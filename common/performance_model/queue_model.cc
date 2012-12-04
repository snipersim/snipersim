#include "queue_model.h"
#include "simulator.h"
#include "config.h"
#include "queue_model_basic.h"
#include "queue_model_history_list.h"
#include "queue_model_contention.h"
#include "queue_model_windowed_mg1.h"
#include "log.h"
#include "config.hpp"

QueueModel*
QueueModel::create(String name, UInt32 id, String model_type, SubsecondTime min_processing_time)
{
   if (model_type == "basic")
   {
      bool moving_avg_enabled = Sim()->getCfg()->getBool("queue_model/basic/moving_avg_enabled");
      UInt32 moving_avg_window_size = Sim()->getCfg()->getInt("queue_model/basic/moving_avg_window_size");
      String moving_avg_type = Sim()->getCfg()->getString("queue_model/basic/moving_avg_type");
      return new QueueModelBasic(name, id, moving_avg_enabled, moving_avg_window_size, moving_avg_type);
   }
   else if (model_type == "history_list")
   {
      return new QueueModelHistoryList(name, id, min_processing_time);
   }
   else if (model_type == "contention")
   {
      return new QueueModelContention(name, id, 1);
   }
   else if (model_type == "windowed_mg1")
   {
      return new QueueModelWindowedMG1(name, id);
   }
   else
   {
      LOG_PRINT_ERROR("Unrecognized Queue Model Type(%s)", model_type.c_str());
      return (QueueModel*) NULL;
   }
}
