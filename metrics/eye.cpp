#include "metrics/eye.hpp"
#include "metrics/eye_serdes.hpp"
#include "metrics/eye_storage.hpp"

#include "platform/platform.hpp"

#include "coding/file_writer.hpp"

#include "base/assert.hpp"
#include "base/logging.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

using namespace eye;

namespace
{
// Three months.
auto constexpr kMapObjectEventsExpirePeriod = std::chrono::hours(24 * 30 * 3);

void Load(Info & info)
{
  Storage::Migrate();

  std::vector<int8_t> infoFileData;
  std::vector<int8_t> mapObjectsFileData;
  if (!Storage::LoadInfo(infoFileData) && !Storage::LoadMapObjects(mapObjectsFileData))
  {
    info = {};
    return;
  }

  try
  {
    if (!infoFileData.empty())
      Serdes::DeserializeInfo(infoFileData, info);

    if (!mapObjectsFileData.empty())
      Serdes::DeserializeMapObjects(mapObjectsFileData, info.m_mapObjects);
  }
  catch (Serdes::UnknownVersion const & ex)
  {
    LOG(LERROR, ("Cannot load metrics files, eye will be disabled. Exception:", ex.Msg()));
    info = {};
  }
}

bool Save(Info const & info)
{
  std::vector<int8_t> fileData;
  Serdes::SerializeInfo(info, fileData);
  return Storage::SaveInfo(fileData);
}

bool SaveMapObjects(MapObjects const & mapObjects)
{
  std::vector<int8_t> fileData;
  Serdes::SerializeMapObjects(mapObjects, fileData);
  return Storage::SaveMapObjects(fileData);
}

bool SaveMapObjectEvent(MapObject const & mapObject, MapObject::Event const & event)
{
  std::vector<int8_t> eventData;
  Serdes::SerializeMapObjectEvent(mapObject, event, eventData);

  return Storage::AppendMapObjectEvent(eventData);
}
}  // namespace

namespace eye
{
Eye::Eye()
{
  Info info;
  Load(info);
  m_info.Set(std::make_shared<Info>(info));

  GetPlatform().RunTask(Platform::Thread::File, [this]
  {
    TrimExpiredMapObjectEvents();
  });
}

// static
Eye & Eye::Instance()
{
  static Eye instance;
  return instance;
}

Eye::InfoType Eye::GetInfo() const
{
  return m_info.Get();
}

void Eye::Subscribe(Subscriber * subscriber)
{
  m_subscribers.push_back(subscriber);
}

void Eye::UnsubscribeAll()
{
  m_subscribers.clear();
}

bool Eye::Save(InfoType const & info)
{
  if (!::Save(*info))
    return false;

  m_info.Set(info);
  return true;
}

void Eye::TrimExpiredMapObjectEvents()
{
  auto const info = m_info.Get();
  auto editableInfo = std::make_shared<Info>(*info);
  auto changed = false;

  for (auto it = editableInfo->m_mapObjects.begin(); it != editableInfo->m_mapObjects.end();)
  {
    auto & events = it->second;
    events.erase(std::remove_if(events.begin(), events.end(), [&changed](auto const & item)
    {
      if (Clock::now() - item.m_eventTime >= kMapObjectEventsExpirePeriod)
      {
        if (!changed)
          changed = true;

        return true;
      }
      return false;
    }), events.end());

    if (events.empty())
      it = editableInfo->m_mapObjects.erase(it);
    else
      ++it;
  }

  if (changed && SaveMapObjects(editableInfo->m_mapObjects))
    m_info.Set(editableInfo);
}

void Eye::RegisterTipClick(Tip::Type type, Tip::Event event)
{
  auto const info = m_info.Get();
  auto editableInfo = std::make_shared<Info>(*info);
  auto & editableTips = editableInfo->m_tips;

  auto it = std::find_if(editableTips.begin(), editableTips.end(), [type](Tip const & tip)
  {
    return tip.m_type == type;
  });

  Tip tip;
  auto const now = Clock::now();
  if (it != editableTips.cend())
  {
    it->m_eventCounters.Increment(event);
    it->m_lastShownTime = now;
    tip = *it;
  }
  else
  {
    tip.m_type = type;
    tip.m_eventCounters.Increment(event);
    tip.m_lastShownTime = now;
    editableTips.push_back(tip);
  }

  if (!Save(editableInfo))
    return;

  GetPlatform().RunTask(Platform::Thread::Gui, [this, tip]
  {
    for (auto subscriber : m_subscribers)
    {
      subscriber->OnTipClicked(tip);
    }
  });
}

void Eye::UpdateBookingFilterUsedTime()
{
  auto const info = m_info.Get();
  auto editableInfo = std::make_shared<Info>(*info);
  auto const now = Clock::now();

  editableInfo->m_booking.m_lastFilterUsedTime = now;

  if (!Save(editableInfo))
    return;

  GetPlatform().RunTask(Platform::Thread::Gui, [this, now]
  {
    for (auto subscriber : m_subscribers)
    {
      subscriber->OnBookingFilterUsed(now);
    }
  });
}

void Eye::UpdateBoomarksCatalogShownTime()
{
  auto const info = m_info.Get();
  auto editableInfo = std::make_shared<Info>(*info);
  auto const now = Clock::now();

  editableInfo->m_bookmarks.m_lastOpenedTime = now;

  if (!Save(editableInfo))
    return;

  GetPlatform().RunTask(Platform::Thread::Gui, [this, now]
  {
    for (auto subscriber : m_subscribers)
    {
      subscriber->OnBookmarksCatalogShown(now);
    }
  });
}

void Eye::UpdateDiscoveryShownTime()
{
  auto const info = m_info.Get();
  auto editableInfo = std::make_shared<Info>(*info);
  auto const now = Clock::now();

  editableInfo->m_discovery.m_lastOpenedTime = now;

  if (!Save(editableInfo))
    return;

  GetPlatform().RunTask(Platform::Thread::Gui, [this, now]
  {
    for (auto subscriber : m_subscribers)
    {
      subscriber->OnDiscoveryShown(now);
    }
  });
}

void Eye::IncrementDiscoveryItem(Discovery::Event event)
{
  auto const info = m_info.Get();
  auto editableInfo = std::make_shared<Info>(*info);

  editableInfo->m_discovery.m_lastClickedTime = Clock::now();
  editableInfo->m_discovery.m_eventCounters.Increment(event);

  if (!Save(editableInfo))
    return;

  GetPlatform().RunTask(Platform::Thread::Gui, [this, event]
  {
    for (auto subscriber : m_subscribers)
    {
      subscriber->OnDiscoveryItemClicked(event);
    }
  });
}

void Eye::RegisterLayerShown(Layer::Type type)
{
  auto const info = m_info.Get();
  auto editableInfo = std::make_shared<Info>(*info);
  auto & editableLayers = editableInfo->m_layers;

  auto it = std::find_if(editableLayers.begin(), editableLayers.end(), [type](Layer const & layer)
  {
    return layer.m_type == type;
  });

  Layer layer;
  if (it != editableLayers.end())
  {
    ++it->m_useCount;
    it->m_lastTimeUsed = Clock::now();
    layer = *it;
  }
  else
  {
    layer.m_type = type;

    ++layer.m_useCount;
    layer.m_lastTimeUsed = Clock::now();
    editableLayers.emplace_back(layer);
  }

  if (!Save(editableInfo))
    return;

  GetPlatform().RunTask(Platform::Thread::Gui, [this, layer]
  {
    for (auto subscriber : m_subscribers)
    {
      subscriber->OnLayerShown(layer);
    }
  });
}

void Eye::RegisterMapObjectEvent(MapObject const & mapObject, MapObject::Event::Type type,
                                 ms::LatLon const & userPos)
{
  auto const info = m_info.Get();
  auto editableInfo = std::make_shared<Info>(*info);
  auto & mapObjects = editableInfo->m_mapObjects;

  MapObject::Event event;
  event.m_type = type;
  event.m_userPos = userPos;
  event.m_eventTime = Clock::now();

  MapObject::Events events;
  auto it = mapObjects.find(mapObject);
  if (it == mapObjects.end())
  {
    events = {event};
    mapObjects.emplace(mapObject, std::move(events));
  }
  else
  {
    it->second.push_back(event);
    events = it->second;
  }

  if (!SaveMapObjectEvent(mapObject, event))
    return;

  m_info.Set(editableInfo);
  GetPlatform().RunTask(Platform::Thread::Gui, [this, mapObject, events]
  {
    for (auto subscriber : m_subscribers)
    {
      subscriber->OnMapObjectEvent(mapObject, events);
    }
  });
}

// Eye::Event methods ------------------------------------------------------------------------------
// static
void Eye::Event::TipClicked(Tip::Type type, Tip::Event event)
{
  GetPlatform().RunTask(Platform::Thread::File, [type, event]
  {
    Instance().RegisterTipClick(type, event);
  });
}

// static
void Eye::Event::BookingFilterUsed()
{
  GetPlatform().RunTask(Platform::Thread::File, []
  {
    Instance().UpdateBookingFilterUsedTime();
  });
}

// static
void Eye::Event::BoomarksCatalogShown()
{
  GetPlatform().RunTask(Platform::Thread::File, []
  {
    Instance().UpdateBoomarksCatalogShownTime();
  });
}

// static
void Eye::Event::DiscoveryShown()
{
  GetPlatform().RunTask(Platform::Thread::File, []
  {
    Instance().UpdateDiscoveryShownTime();
  });
}

// static
void Eye::Event::DiscoveryItemClicked(Discovery::Event event)
{
  GetPlatform().RunTask(Platform::Thread::File, [event]
  {
    Instance().IncrementDiscoveryItem(event);
  });
}

// static
void Eye::Event::LayerShown(Layer::Type type)
{
  GetPlatform().RunTask(Platform::Thread::File, [type]
  {
    Instance().RegisterLayerShown(type);
  });
}

// static
void Eye::Event::PlacePageOpened(std::string const & bestType, ms::LatLon const & latLon,
                                 ms::LatLon const & userPos)
{
  GetPlatform().RunTask(Platform::Thread::File, [bestType, latLon, userPos]
  {
    Instance().RegisterMapObjectEvent({bestType, latLon}, MapObject::Event::Type::Open, userPos);
  });
}

// static
void Eye::Event::UgcEditorOpened(std::string const & bestType, ms::LatLon const & latLon,
                                 ms::LatLon const & userPos)
{
  GetPlatform().RunTask(Platform::Thread::File, [bestType, latLon, userPos]
  {
    Instance().RegisterMapObjectEvent({bestType, latLon}, MapObject::Event::Type::UgcEditorOpened,
                                      userPos);
  });
}

// static
void Eye::Event::UgcSaved(std::string const & bestType, ms::LatLon const & latLon,
                          ms::LatLon const & userPos)
{
  GetPlatform().RunTask(Platform::Thread::File, [bestType, latLon, userPos]
  {
    Instance().RegisterMapObjectEvent({bestType, latLon}, MapObject::Event::Type::UgcSaved,
                                      userPos);
  });
}

// static
void Eye::Event::AddToBookmarkClicked(std::string const & bestType, ms::LatLon const & latLon,
                                      ms::LatLon const & userPos)
{
  GetPlatform().RunTask(Platform::Thread::File, [bestType, latLon, userPos]
  {
    Instance().RegisterMapObjectEvent({bestType, latLon}, MapObject::Event::Type::AddToBookmark,
                                      userPos);
  });
}

// static
void Eye::Event::RouteCreatedToObject(std::string const & bestType, ms::LatLon const & latLon,
                                      ms::LatLon const & userPos)
{
  GetPlatform().RunTask(Platform::Thread::File, [bestType, latLon, userPos]
  {
    Instance().RegisterMapObjectEvent({bestType, latLon}, MapObject::Event::Type::RouteToCreated,
                                      userPos);
  });
}
}  // namespace eye
