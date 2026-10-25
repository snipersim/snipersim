
#pragma once
#include "fixed_types.h"
#include "cache.h"
#include <unordered_map>
#include <random>
#include "pwc.h"

using namespace std;

namespace ParametricDramDirectoryMSI
{
	typedef tuple<SubsecondTime, vector<IntPtr>, CacheBlockInfo::block_type_t> MetadataWalkResult;

	class MetadataTableBase
	{

	protected:
		int core_id;
		String name;

	public:
		MetadataTableBase(){};
		virtual ~MetadataTableBase() {};
		virtual MetadataWalkResult initializeWalk(IntPtr address) = 0;
	};

}
