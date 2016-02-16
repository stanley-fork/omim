#include "drape_frontend/tile_utils.hpp"

#include "indexer/scales.hpp"

#include "geometry/mercator.hpp"

#include "base/assert.hpp"
#include "base/stl_add.hpp"

namespace df
{

CoverageResult CalcTilesCoverage(m2::RectD const & rect, int targetZoom,
                                 function<void(int, int)> const & processTile)
{
  double const range = MercatorBounds::maxX - MercatorBounds::minX;
  double const rectSize = range / (1 << targetZoom);

  CoverageResult result;
  result.m_minTileX = static_cast<int>(floor(rect.minX() / rectSize));
  result.m_maxTileX = static_cast<int>(ceil(rect.maxX() / rectSize));
  result.m_minTileY = static_cast<int>(floor(rect.minY() / rectSize));
  result.m_maxTileY = static_cast<int>(ceil(rect.maxY() / rectSize));

  if (processTile != nullptr)
  {
    for (int tileY = result.m_minTileY; tileY < result.m_maxTileY; ++tileY)
      for (int tileX = result.m_minTileX; tileX < result.m_maxTileX; ++tileX)
        processTile(tileX, tileY);
  }

  return result;
}

bool IsNeighbours(TileKey const & tileKey1, TileKey const & tileKey2)
{
  return !((tileKey1.m_x == tileKey2.m_x) && (tileKey1.m_y == tileKey2.m_y)) &&
         (abs(tileKey1.m_x - tileKey2.m_x) < 2) &&
         (abs(tileKey1.m_y - tileKey2.m_y) < 2);
}

int ClipTileZoomByMaxDataZoom(int zoom)
{
  int const upperScale = scales::GetUpperScale();
  return zoom <= upperScale ? zoom : upperScale;
}

} // namespace df
