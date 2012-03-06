#ifndef LOOP_BRANCH_PREDICTOR_H
#define LOOP_BRANCH_PREDICTOR_H

#include <vector>
#include <stdint.h>

#include "simulator.h"
#include "branch_predictor.h"
#include "branch_predictor_return_value.h"
#include "saturating_predictor.h"

#define DEBUG 0

#if DEBUG == 0
# define debug_cout if (0) std::cout
#else
# define debug_cout std::cout
#endif

class LoopBranchPredictor
{

public:

   LoopBranchPredictor(UInt32 entries, UInt32 tag_bitwidth, UInt32 ways)
      : m_lru_use_count(0)
      , m_num_ways(ways)
      , m_ways(ways, Way(entries/ways, tag_bitwidth))
   {
   }

   // Not sure if predicted can be used
   //  it comes from a higher hierarchy
   //  our predictor isn't valid for this implementation
   bool predict(IntPtr ip, IntPtr target)
   {
      // A IP => predictor is undefined for the LPB because it provides both a hit and prediction
      // (prediction is only valid when there is a hit)
      return false;
   }
   BranchPredictorReturnValue lookup(IntPtr ip, IntPtr target)
   {

      UInt32 index, tag;
      BranchPredictorReturnValue ret = { 0, 0, 0, BranchPredictorReturnValue::InvalidBranch };

      gen_index_tag(ip, index, tag);

      for (unsigned int w = 0 ; w < m_num_ways ; ++w )
      {
         // When we are enabled, and we hit, we can use the value even if the count isn't set to the limit
         if ( m_ways[w].m_enabled[index]
           && m_ways[w].m_tags[index] == tag )
         {
            UInt32 count = m_ways[w].m_count[index];
            UInt32 limit = m_ways[w].m_limit[index];

            ret.hit = 1;
            // 000001 -> predict() == 0; 111110 -> predict() == 1
            if (count == limit)
            {
               ret.prediction = ! m_ways[w].m_predictors[index].predict();
            }
            else
            {
               ret.prediction = m_ways[w].m_predictors[index].predict();
            }
            // Save the lru data
            m_ways[w].m_lru[index] = m_lru_use_count++;
            break;
         }
      }

      return ret;
   }

   // 000001 -> predict() == 0; 111110 -> predict() == 1
   void update(bool predicted, bool actual, IntPtr ip, IntPtr target)
   {

      UInt32 index, tag, lru_way;

      gen_index_tag(ip, index, tag);

      // Start with way 0 as the least recently used
      lru_way = 0;

      for (UInt32 w = 0 ; w < m_num_ways ; ++w )
      {
         if (m_ways[w].m_tags[index] == tag)
         {

            bool current_prediction = m_ways[w].m_predictors[index].predict();
            bool match = prediction_match(w, index, actual);
            bool previous_actual = m_ways[w].m_previous_actual[index];
            UInt32 &next_counter = m_ways[w].m_count[index];
            UInt32 &next_limit = m_ways[w].m_limit[index];
            uint8_t &next_enabled = m_ways[w].m_enabled[index];
            UInt32 current_counter = next_counter;
            UInt32 current_limit = next_limit;
            uint8_t current_enabled = next_enabled;

            // First, determine the control logic for when we are moving in the same direction
            //  When we see a change, update the limit and reset the counter
            //  If we are changing our prediction (000001 to 111110), this logic won't work,
            //   and will be overwritten by the additional logic below

            UInt32 same_direction_counter;
            UInt32 same_direction_limit;
            uint8_t same_direction_enabled = next_enabled;

            // Update the limit if we have mismatched
            if ( ! match )
            {
               // Always update the counter on a mismatch

               debug_cout << "[SAME-DIRECTION] Mismatch detected" << std::endl;

               // If the new loop length is now shorter, update the limit with the new counter (loop length)
               // Reset the counter because we are starting a new loop
               if (current_counter < current_limit)
               {
                  debug_cout << "[SAME-DIRECTION] Mismatch: Shorter loop found" << std::endl;

                  same_direction_counter = 0;
                  same_direction_limit = current_counter;
               }
               // Otherwise, the loop length is now longer, increment the counter and the limit
               // Do not reset the counter yet
               // Since this loop is now longer, we should match sometime in the future
               else
               {
                  same_direction_counter = current_counter + 1;
                  same_direction_limit = current_counter + 1;
                  same_direction_enabled = 0;

                  debug_cout << "[SAME-DIRECTION] Mismatch: Longer loop found [C:L] [" << current_counter << ":" << current_limit << "] => [" << same_direction_counter << ":" << same_direction_limit << "]" << std::endl;
               }
            }
            else
            {
               debug_cout << "[SAME-DIRECTION] Match detected" << std::endl;

               // On a match, we can continue as normal
               // Update the counter, and nothing else
               // If we have reached the limit, reset the counter
               if (current_counter == current_limit)
                  same_direction_counter = 0;
               else
                  same_direction_counter = current_counter + 1;

               // The limit will remain the same
               same_direction_limit = current_limit;
            }

            // Determine if we have changed our state
            // This happens when we have seen two branches both as either taken or not-taken,
            //  and that this branch status was different than the type predicted.
            //  000001 -> predict() == 0; 111110 -> predict() == 1
            //  Example: For the 000001 case (0), we do not expect to see two 1's in a row.
            if (previous_actual == actual && actual == !current_prediction)
            {

               debug_cout << "[CHANGE-DIRECTION] Detected!" << std::endl;

               // Here, we have seen two of the opposite type in a row
               // Reset the limit and the counter for the next pass
               // We assume that the two predictions seen here are not part of the same branch
               //  structure, but of two seperate branch structures, and therefore count it as one loop
               //  iteration instead of two
               next_counter = 1;
               next_limit = 1;

               // Update the predictor
               //  For the 000001 (0) case, and we've seen two 1's, set the predictor to (1), ie. 111110
               m_ways[w].m_predictors[index].update(actual);

               // Disable the entry since we have just started to look in another direction
               next_enabled = false;
            }
            else
            {

               debug_cout << "[NO-CHANGE-DIR] Not Detected!" << std::endl;

               // Determine if we should enable this prediction entry
               // This should happen when we first see a string of branches
               //  and then have one in the opposite direction
               //  and we are following our current prediction direction
               // This situation is also seen in the normal same-direction
               //  processing.  If that is the case, continue as normal
               // Therefore, only reset the counters if we are not currently
               //  enabled.  Already enabled branches should continue as they are
               if (previous_actual != actual && actual == !current_prediction)
               {
                  if (! current_enabled)
                  {
                     debug_cout << "[NO-CHANGE-DIR] Enabling entry" << std::endl;
                     next_enabled = true;
                     // At the same time, setup the count and limits correctly
                     next_counter = 0;
                     next_limit = current_limit;
                  }
                  else
                  {
                     debug_cout << "[NO-CHANGE-DIR] Entry already enabled" << std::endl;

                     // Here, we haven't see a direction change.
                     // Use the state from the previous, same direction, section
                     next_enabled = same_direction_enabled;
                     next_counter = same_direction_counter;
                     next_limit = same_direction_limit;

                  }
               }
               else
               {
               // We are continuing along the normal prediction path, simply update with the next state

                  debug_cout << "[NO-CHANGE-DIR] Updating entry with same direction [C:L] [" << current_counter << ":" << current_limit << "] => [" << same_direction_counter << ":" << same_direction_limit << "]" << std::endl;

                  // Here, we haven't see a direction change.
                  // Use the state from the previous, same direction, section
                  next_enabled = same_direction_enabled;
                  next_counter = same_direction_counter;
                  next_limit = same_direction_limit;

               }

               // Do not update the actual predictor, since we are moving in the same loop direction
            }


            // Update state and LRU for our next branch
            m_ways[w].m_previous_actual[index] = actual;
            m_ways[w].m_lru[index] = m_lru_use_count++;
            // Once we have a tag match and have updated the LRU information,
            // we can return
            return;
         }

         // Keep track of the LRU in case we do not have a tag match
         if (m_ways[w].m_lru[index] < m_ways[lru_way].m_lru[index])
         {
            lru_way = w;
         }
      }

      // We will get here only if we have not matched the tag
      // If that is the case, select the LRU entry, and update the tag
      // appropriately
      m_ways[lru_way].m_tags[index] = tag;
      // Here, we miss with the tag, so reset instead of updating
      m_ways[lru_way].m_predictors[index].reset(actual);
      m_ways[lru_way].m_previous_actual[index] = actual;
      m_ways[lru_way].m_lru[index] = m_lru_use_count++;
      m_ways[lru_way].m_count[index] = 1;
      m_ways[lru_way].m_limit[index] = 1;

   }

private:

   class Way
   {
   public:

      Way(UInt32 entries, UInt32 tag_bitwidth)
         : m_tags(entries, 0)
         , m_previous_actual(entries, 0)
         , m_enabled(entries, 0)
         , m_predictors(entries, SaturatingPredictor<1>(0))
         , m_lru(entries, 0)
         , m_count(entries, 0)
         , m_limit(entries, 0)
         , m_num_entries(entries)
         , m_tag_bitwidth(tag_bitwidth)
      {
         assert(tag_bitwidth <= 8);
      }

      std::vector<uint8_t> m_tags;
      std::vector<uint8_t> m_previous_actual;
      std::vector<uint8_t> m_enabled;
      std::vector<SaturatingPredictor<1> > m_predictors;
      std::vector<UInt64> m_lru;
      std::vector<UInt32> m_count;
      std::vector<UInt32> m_limit;
      UInt32 m_num_entries;
      UInt32 m_tag_bitwidth;

   };

   // Pentium M-specific indexing and tag values
   void gen_index_tag(IntPtr ip, UInt32& index, UInt32 &tag)
   {
      index = (ip >> 4) & 0x3F;
      tag = (ip >> 10) & 0x3F;
   }

   // 000001 -> predict() == 0; 111110 -> predict() == 1
   inline bool prediction_match(UInt32 way, UInt32 index, bool actual)
   {

      bool prediction = m_ways[way].m_predictors[index].predict();
      UInt32 count = m_ways[way].m_count[index];
      UInt32 limit = m_ways[way].m_limit[index];

      // At our count limit
      if (count == limit)
      {
         // Did we predict correctly?
         if (prediction != actual)
         {
            debug_cout << "[PRED-MATCH] Predicted the Loop taken/not-taken limit" << std::endl;
            return true;
         }
         else
            return false;
      }
      // Not at our count limit
      else
      {
         // Did we predict correctly?
         if (prediction == actual)
            return true;
         else
            return false;
      }
   }

   UInt64 m_lru_use_count;
   UInt32 m_num_ways;
   std::vector<Way> m_ways;

};

#endif /* LOOP_BRANCH_PREDICTOR_H */
