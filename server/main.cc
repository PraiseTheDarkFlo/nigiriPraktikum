#include <filesystem>
#include <fstream>
#include <ostream>
#include <random>
#include <vector>

#include "fmt/chrono.h"
#include "fmt/ranges.h"

#include "date/date.h"

#include "cista/hash.h"

#include "utl/enumerate.h"
#include "utl/helpers/algorithm.h"
#include "utl/progress_tracker.h"
#include "utl/to_vec.h"
#include "utl/verify.h"

#include "nigiri/loader/gtfs/loader.h"
#include "nigiri/loader/hrd/loader.h"
#include "nigiri/loader/init_finish.h"
#include "nigiri/logging.h"
#include "nigiri/routing/reach.h"
#include "nigiri/timetable.h"
#include "geo/box.h"
#include "utl/erase_if.h"

using namespace date;
using namespace nigiri;
using namespace nigiri::loader;
namespace fs = std::filesystem;

constexpr auto const kTimetablePath = "timetable.bin";

date::sys_days parse_date(std::string_view s) {
  date::sys_days date;
  std::stringstream ss;
  ss << s;
  ss >> date::parse("%F", date);
  return date;
}

int load(interval<date::sys_days> const date_range,
         std::vector<std::string_view> const& paths) {
  fmt::print("range={}, paths={}", date_range, paths);

  auto loaders = std::vector<std::unique_ptr<loader_interface>>{};
  loaders.emplace_back(std::make_unique<gtfs::gtfs_loader>());
  loaders.emplace_back(std::make_unique<hrd::hrd_5_00_8_loader>());
  loaders.emplace_back(std::make_unique<hrd::hrd_5_20_26_loader>());
  loaders.emplace_back(std::make_unique<hrd::hrd_5_20_39_loader>());
  loaders.emplace_back(std::make_unique<hrd::hrd_5_20_avv_loader>());

  auto datasets =
      std::vector<std::pair<std::size_t, std::unique_ptr<loader::dir>>>{};
  for (auto const& p : paths) {
    auto d = loader::make_dir(p);
    auto const c =
        utl::find_if(loaders, [&](auto&& l) { return l->applicable(*d); });
    utl::verify(c != end(loaders), "no loader applicable to {}", p);
    datasets.emplace_back(
        static_cast<std::size_t>(std::distance(begin(loaders), c)),
        std::move(d));
  }

  auto const bars = utl::global_progress_bars{};

  timetable tt;
  tt.date_range_ = date_range;
  register_special_stations(tt);
  for (auto const [src, dataset] : utl::enumerate(datasets)) {
    auto progress_tracker =
        utl::activate_progress_tracker(fmt::format("{}", paths[src]));
    auto const& [loader_idx, dir] = dataset;
    loaders[loader_idx]->load({}, source_idx_t{src}, *dir, tt);
  }
  finalize(tt);

  tt.write(kTimetablePath);

  return 0;
}

int reach(interval<date::sys_days> const date_range) {
  std::cout << "Loading timetable..." << std::flush;
  auto tt_wrapped = cista::wrapped<timetable>{timetable::read(
      cista::memory_holder{cista::file{kTimetablePath, "r"}.content()})};
  auto& tt = *tt_wrapped;
  tt_wrapped->locations_.resolve_timezones();
  std::cout << " done" << std::endl;

  std::cout << "Generating locations..." << std::flush;
  std::vector<location_idx_t> source_locations;
  source_locations.resize(tt.n_locations());
  std::generate(begin(source_locations), end(source_locations),
                [i = location_idx_t{0U}]() mutable { return i++; });
  std::cout << " done" << std::endl;

  std::cout << "Shuffle..." << std::flush;
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(begin(source_locations), end(source_locations), g);
  source_locations.resize(10);
  std::cout << " done" << std::endl;

  auto const bars = utl::global_progress_bars{};

  std::cout << "Compute route reachs..." << std::flush;
  auto const route_reachs =
      routing::get_reach_values(tt, source_locations, date_range);
  std::cout << " done" << std::endl;

  std::vector<double> route_bbox{};
  route_bbox.resize(tt.n_routes());
  for (auto r = route_idx_t{0U}; r != tt.n_routes(); ++r) {
    auto b = geo::box{};
    auto const location_seq = tt.route_location_seq_[route_idx_t{r}];
    for (auto i = 0U; i != location_seq.size(); ++i) {
      auto const l = stop{location_seq[i]}.location_idx();
      b.extend(tt.locations_.coordinates_[l]);
    }
    route_bbox[to_idx(r)] = geo::distance(b.max_, b.min_);
  }

  auto f = std::ofstream{"reach.txt"};
  for (auto const& [r, reach] : utl::enumerate(route_reachs)) {
    if (reach.valid()) {
      f << reach.reach_ << " " << route_bbox[r] << "\n";
    }
  }
  auto perm = std::vector<unsigned>{};
  perm.resize(route_reachs.size());
  std::generate(begin(perm), end(perm), [i = 0U]() mutable { return i++; });

  utl::erase_if(perm, [&](unsigned const r) { return route_bbox[r] > 30'000; });
  utl::sort(perm, [&](unsigned const a, unsigned const b) {
    return route_reachs[a].reach_ > route_reachs[b].reach_;
  });

  for (auto i = 0U; i != std::min(static_cast<std::size_t>(10U), perm.size());
       ++i) {
    if (i == route_reachs.size()) {
      break;
    }
    auto const r = perm[i];
    auto const& info = route_reachs[r];

    auto const stop_seq = tt.route_location_seq_[route_idx_t{r}];
    for (auto const s : stop_seq) {
      std::cout << location{tt, stop{s}.location_idx()} << " ";
    }
    std::cout << "\n";

    std::cout << "reach: " << info.reach_ << "\n";
    info.j_.print(std::cout, tt);
    std::cout << "\n\n";
  }

  return 0;
}

int main(int ac, char** av) {
  auto const print_usage = [&]() {
    fmt::print(
        "usage: {}"
        "   load {from_date} {to_date} {timetable_path ...}\n"
        "   reach {from_date} {to_date}\n",
        ac == 0U ? "nigiri-server" : av[0]);
  };

  if (ac < 2) {
    return 1;
  }

  auto const cmd = std::string_view{av[1]};
  switch (cista::hash(cmd)) {
    case cista::hash("load"): {
      if (ac < 5) {
        print_usage();
        return 1;
      }
      return load({parse_date(av[2]), parse_date(av[3])},
                  utl::to_vec(it_range{av + 4, av + ac},
                              [](char* p) { return std::string_view{p}; }));
    }

    case cista::hash("reach"):
      return reach({parse_date(av[2]), parse_date(av[3])});
      break;

    default: fmt::print("unknown command {}\n", cmd); return 1;
  }
}