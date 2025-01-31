/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "dnsname.hh"
#include "lock.hh"
#include "misc.hh"

class AuthZoneCache : public boost::noncopyable
{
public:
  AuthZoneCache(size_t mapsCount = 1024);

  void replace(const vector<std::tuple<DNSName, int>>& zone);
  void add(const DNSName& zone, const int zoneId);
  void remove(const DNSName& zone);
  void setReplacePending(); //!< call this when data collection for the subsequent replace() call starts.

  bool getEntry(const DNSName& zone, int& zoneId);

  size_t size() { return *d_statnumentries; } //!< number of entries in the cache

  uint32_t getRefreshInterval() const
  {
    // coverity[store_truncates_time_t]
    return d_refreshinterval;
  }

  void setRefreshInterval(uint32_t interval)
  {
    d_refreshinterval = interval;
  }

  bool isEnabled() const;

  void clear();

private:
  struct CacheValue { int zoneId{-1}; };
  /**
   * 还需要给他提供一个 hash 函数
   * 这部部分工作, 在 C#/Java中依靠 override GetHashCode() 方法来实现
   * 然而，对于自定义类型（如 DNSName），如果没有为其定义默认的哈希函数，编译器会报错，因为它不知道如何为该类型生成哈希值。
   */
  typedef std::unordered_map<DNSName, CacheValue, std::hash<DNSName>> cmap_t;

  struct MapCombo
  {
    MapCombo() = default;
    ~MapCombo() = default;

    // 零成本抽象, 通过 delete 关键字禁用拷贝构造函数和赋值运算符, C++ 太细了
    MapCombo(const MapCombo&) = delete; // 禁用赋值运算符，这意味着 MapCombo 对象不能被赋值给另一个 MapCombo 对象。
    MapCombo& operator=(const MapCombo&) = delete; // 禁用拷贝构造函数，这意味着 MapCombo 对象不能通过拷贝另一个 MapCombo 对象来创建

    SharedLockGuarded<cmap_t> d_map;
  };

  vector<MapCombo> d_maps; // 还是两层 Hash 吗? 先链后表
  size_t getMapIndex(const DNSName& zone) { return zone.hash() % d_maps.size(); }
  MapCombo& getMap(const DNSName& qname) { return d_maps[getMapIndex(qname)]; }

  AtomicCounter* d_statnumhit;
  AtomicCounter* d_statnummiss;
  AtomicCounter* d_statnumentries;

  time_t d_refreshinterval{0};

  struct PendingData
  {
    std::vector<std::tuple<DNSName, int, bool>> d_pendingUpdates;
    bool d_replacePending{false};
  };
  LockGuarded<PendingData> d_pending;
};

extern AuthZoneCache g_zoneCache;
