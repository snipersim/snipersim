#include "simulator.h"
#include "branch_predictor.h"
#include "one_bit_branch_predictor.h"
#include "pentium_m_branch_predictor.h"
#include "a53branchpredictor.h"
#include "gshare_branch_predictor.h"
#include "perceptron_branch_predictor.h"
#include "hashed_perceptron_branch_predictor.h"
#include "tage_branch_predictor.h"
#if HAS_LIBTORCH
#include "nn_branch_predictor.h"
#endif
#include "config.hpp"
#include "stats.h"

BranchPredictor::BranchPredictor()
   : m_correct_predictions(0)
   , m_incorrect_predictions(0)
{
}

BranchPredictor::BranchPredictor(String name, core_id_t core_id)
   : m_correct_predictions(0)
   , m_incorrect_predictions(0)
{
  registerStatsMetric(name, core_id, "num-correct", &m_correct_predictions);
  registerStatsMetric(name, core_id, "num-incorrect", &m_incorrect_predictions);
}

BranchPredictor::~BranchPredictor()
{ }

UInt64 BranchPredictor::m_mispredict_penalty;

BranchPredictor* BranchPredictor::create(core_id_t core_id)
{
   try
   {
      config::Config *cfg = Sim()->getCfg();
      assert(cfg);

      m_mispredict_penalty = cfg->getIntArray("perf_model/branch_predictor/mispredict_penalty", core_id);

      String type = cfg->getStringArray("perf_model/branch_predictor/type", core_id);
      if (type == "none")
      {
         return 0;
      }
      else if (type == "one_bit")
      {
         UInt32 size = cfg->getIntArray("perf_model/branch_predictor/size", core_id);
         return new OneBitBranchPredictor("branch_predictor", core_id, size);
      }
      else if (type == "pentium_m")
      {
         return new PentiumMBranchPredictor("branch_predictor", core_id);
      }
      else if (type == "a53") {
          return new A53BranchPredictor("branch_predictor", core_id);
      }
      // Step 26 of the Virtuoso port series: ChampSim-style predictors.
      // Their sizes are optional knobs with the published defaults.
      else if (type == "gshare")
      {
         UInt32 history_length = cfg->hasKey("perf_model/branch_predictor/gshare/history_length")
            ? cfg->getIntArray("perf_model/branch_predictor/gshare/history_length", core_id) : 14;
         UInt32 table_size = cfg->hasKey("perf_model/branch_predictor/gshare/table_size")
            ? cfg->getIntArray("perf_model/branch_predictor/gshare/table_size", core_id) : 16384;
         return new GshareBranchPredictor("branch_predictor", core_id, history_length, table_size);
      }
      else if (type == "perceptron")
      {
         UInt32 history_length = cfg->hasKey("perf_model/branch_predictor/perceptron/history_length")
            ? cfg->getIntArray("perf_model/branch_predictor/perceptron/history_length", core_id) : 24;
         UInt32 num_perceptrons = cfg->hasKey("perf_model/branch_predictor/perceptron/num_perceptrons")
            ? cfg->getIntArray("perf_model/branch_predictor/perceptron/num_perceptrons", core_id) : 163;
         return new PerceptronBranchPredictor("branch_predictor", core_id, history_length, num_perceptrons);
      }
      else if (type == "hashed_perceptron")
      {
         return new HashedPerceptronBranchPredictor("branch_predictor", core_id);
      }
      else if (type == "tage")
      {
         return new TAGEBranchPredictor("branch_predictor", core_id);
      }
#if HAS_LIBTORCH
      else if (type == "nn")
      {
         // The class was written, compiled and linked, but never constructed:
         // the factory had no case for it, so perf_model/branch_predictor/type=nn
         // -- which test/fft-nn asks for -- failed with "Invalid branch
         // predictor type".
         size_t batch_length = cfg->getIntArray("perf_model/branch_predictor/batch_length", core_id);
         double learning_rate = cfg->getFloatArray("perf_model/branch_predictor/learning_rate", core_id);
         return new NNBranchPredictor("branch_predictor", core_id, batch_length, learning_rate);
      }
#endif
      else
      {
         LOG_PRINT_ERROR("Invalid branch predictor type.");
         return 0;
      }
   }
   catch (...)
   {
      LOG_PRINT_ERROR("Config info not available while constructing branch predictor.");
      return 0;
   }
}

UInt64 BranchPredictor::getMispredictPenalty()
{
   return m_mispredict_penalty;
}

void BranchPredictor::resetCounters()
{
  m_correct_predictions = 0;
  m_incorrect_predictions = 0;
}

void BranchPredictor::updateCounters(bool predicted, bool actual)
{
   if (predicted == actual)
      ++m_correct_predictions;
   else
      ++m_incorrect_predictions;
}
