#include "nigiri/routing/raptor/raptor_state.h"

#include <algorithm>

#include "fmt/core.h"

#include "utl/helpers/algorithm.h"

#include "nigiri/routing/limits.h"
#include "nigiri/timetable.h"

namespace nigiri::routing {

void raptor_state::resize(unsigned const n_locations,
                          unsigned const n_routes,
                          unsigned const n_rt_transports) {
  tmp_.resize(n_locations);
  station_mark_.resize(n_locations);
  prev_station_mark_.resize(n_locations);
  route_mark_.resize(n_routes);
  best_.resize(n_locations);
  round_times_.resize(kMaxTransfers + 1U, n_locations);
  rt_transport_mark_.resize(n_rt_transports);
  end_reachable_.resize(n_locations);
}

void raptor_state::print(timetable const& tt,
                         date::sys_days const base,
                         delta_t const invalid) {
  auto invalid_array = std::array<delta_t, kMaxVias + 1>{};
  invalid_array.fill(invalid);

  auto const has_empty_rounds = [&](std::uint32_t const l) {
    for (auto k = 0U; k != kMaxTransfers + 1U; ++k) {
      if (round_times_[k][l] != invalid_array) {
        return false;
      }
    }
    return true;
  };

  auto const print_delta = [&](delta_t const d) {
    if (d == invalid) {
      fmt::print("________________");
    } else {
      fmt::print("{:16}", delta_to_unix(base, d));
    }
  };

  auto const print_deltas =
      [&](std::array<delta_t, kMaxVias + 1> const& deltas) {
        fmt::print("[ ");
        for (auto const d : deltas) {
          print_delta(d);
          fmt::print(" ");
        }
        fmt::print("]");
      };

  for (auto l = 0U; l != tt.n_locations(); ++l) {
    if (best_[l] == invalid_array && has_empty_rounds(l)) {
      continue;
    }

    fmt::print("{:80}  ", location{tt, location_idx_t{l}});

    fmt::print("tmp=");
    print_deltas(tmp_[l]);
    fmt::print(", ");

    auto const& b = best_[l];
    fmt::print("best=");
    print_deltas(b);
    fmt::print(", round_times: ");
    for (auto i = 0U; i != kMaxTransfers + 1U; ++i) {
      auto const& t = round_times_[i][l];
      fmt::print("{}:", i);
      print_deltas(t);
      fmt::print(" ");
    }
    fmt::print("\n");
  }
}

}  // namespace nigiri::routing
