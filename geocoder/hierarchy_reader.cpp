#include "geocoder/hierarchy_reader.hpp"

#include "base/logging.hpp"

#include <thread>

using namespace std;

namespace geocoder
{
namespace
{
// Information will be logged for every |kLogBatch| entries.
size_t const kLogBatch = 100000;
} // namespace

HierarchyReader::HierarchyReader(string const & pathToJsonHierarchy) :
  m_fileStm{pathToJsonHierarchy}
{
  if (!m_fileStm)
    MYTHROW(OpenException, ("Failed to open file", pathToJsonHierarchy));
}

vector<Hierarchy::Entry> HierarchyReader::ReadEntries(size_t readersCount, ParsingStats & stats)
{
  LOG(LINFO, ("Reading entries..."));

  readersCount = min(readersCount, size_t{8});
  vector<multimap<base::GeoObjectId, Entry>> taskEntries(readersCount);
  vector<thread> tasks{};
  for (size_t t = 0; t < readersCount; ++t)
    tasks.emplace_back(&HierarchyReader::ReadEntryMap, this, ref(taskEntries[t]), ref(stats));

  for (auto & reader : tasks)
    reader.join();

  if (stats.m_numLoaded % kLogBatch != 0)
    LOG(LINFO, ("Read", stats.m_numLoaded, "entries"));

  return UnionEntries(taskEntries);
}

vector<Hierarchy::Entry> HierarchyReader::UnionEntries(vector<multimap<base::GeoObjectId, Entry>> & entryParts)
{
  auto entries = vector<Entry>{};

  size_t size{0};
  for (auto const & map : entryParts)
    size += map.size();

  entries.reserve(size);

  LOG(LINFO, ("Sorting entries..."));

  while (entryParts.size())
  {
    auto minPart = min_element(entryParts.begin(), entryParts.end());

    if (minPart->size())
    {
      entries.emplace_back(move(minPart->begin()->second));
      minPart->erase(minPart->begin());
    }

    if (minPart->empty())
      entryParts.erase(minPart);
  }

  return entries;
}

void HierarchyReader::ReadEntryMap(multimap<base::GeoObjectId, Entry> & entries, ParsingStats & stats)
{
  // Temporary local object for efficient concurent processing (individual cache line for container).
  auto localEntries = multimap<base::GeoObjectId, Entry>{};

  string line;
  while (true)
  {
    {
      auto && lock = lock_guard<mutex>(m_mutex);
  
      if (!getline(m_fileStm, line))
        break;
    }
        
    if (line.empty())
      continue;

    auto const i = line.find(' ');
    int64_t encodedId;
    if (i == string::npos || !strings::to_any(line.substr(0, i), encodedId))
    {
      LOG(LWARNING, ("Cannot read osm id. Line:", line));
      ++stats.m_badOsmIds;
      continue;
    }
    line = line.substr(i + 1);

    Entry entry;
    // todo(@m) We should really write uints as uints.
    auto const osmId = base::GeoObjectId(static_cast<uint64_t>(encodedId));
    entry.m_osmId = osmId;

    if (!entry.DeserializeFromJSON(line, stats))
      continue;

    if (entry.m_type == Type::Count)
      continue;

    ++stats.m_numLoaded;
    if (stats.m_numLoaded % kLogBatch == 0)
      LOG(LINFO, ("Read", (stats.m_numLoaded / kLogBatch) * kLogBatch, "entries"));

    localEntries.emplace(osmId, move(entry));
  }

  entries = move(localEntries);
}
} // namespace geocoder
