#include <alpaka/core/Common.hpp>
#include <cstdint>

#include "../AlpakaCore/alpakaWorkDiv.h"
#include "../DataFormats/alpaka/PointsAlpaka.h"
#include "../DataFormats/alpaka/TilesAlpaka.h"
#include "../DataFormats/alpaka/AlpakaVecArray.h"

using cms::alpakatools::VecArray;

namespace ALPAKA_ACCELERATOR_NAMESPACE {

  template <uint8_t Ndim>
  using PointsView = typename PointsAlpaka<Ndim>::PointsAlpakaView;

  struct KernelFillTiles {
    template <typename TAcc, uint8_t Ndim>
    ALPAKA_FN_ACC void operator()(const TAcc& acc,
                                  PointsView<Ndim>* points,
                                  TilesAlpaka<TAcc, Ndim>* tiles,
                                  uint32_t n_points) const {
      cms::alpakatools::for_each_element_in_grid(
          acc, n_points, [&](uint32_t i) { tiles->fill(acc, points->coords[i], i); });
    }
  };

  template <typename TAcc, uint8_t Ndim, uint8_t N_>
  ALPAKA_FN_HOST_ACC void for_recursion(VecArray<uint32_t, Ndim>& base_vec,
                                        const VecArray<VecArray<uint32_t, 2>, Ndim>& search_box,
                                        TilesAlpaka<TAcc, Ndim>* tiles,
                                        PointsView<Ndim>* dev_points,
                                        const VecArray<float, Ndim>& point_coordinates,
                                        float* rho_i,
                                        float dc,
                                        int i) {
    if constexpr (N_ == 0) {
      int binId{tiles->getGlobalBinByBin(base_vec)};
      // get the size of this bin
      int binSize{static_cast<int>(tiles[binId].size())};

      // iterate inside this bin
      for (int binIter{}; binIter < binSize; ++binIter) {
        int j{tiles[binId][binIter]};
        // query N_{dc_}(i)

        VecArray<float, Ndim> j_coords{dev_points[j]};
        float dist_ij_sq{0.f};
        for (int dim{}; dim != Ndim; ++dim) {
          dist_ij_sq += (j_coords[dim] - point_coordinates[dim]) * (j_coords[dim] - point_coordinates[dim]);
        }

        if (dist_ij_sq <= dc * dc) {
          *rho_i += (i == j ? 1.f : 0.5f);
        }
      }  // end of interate inside this bin

      return;
    } else {
      for (int i{search_box[search_box.size() - N_][0]}; i <= search_box[search_box.size() - N_][1]; ++i) {
        base_vec[base_vec.size() - N_] = i;
        for_recursion<TAcc, Ndim, N_ - 1>(base_vec, search_box, tiles, dev_points, point_coordinates, rho_i, i);
      }
    }
  }

  struct KernelCalculateLocalDensity {
    template <typename TAcc, uint8_t Ndim>
    ALPAKA_FN_ACC void operator()(const TAcc& acc,
                                  TilesAlpaka<TAcc, Ndim>* dev_tiles,
                                  PointsView<Ndim>* dev_points,
                                  float dc,
                                  uint32_t n_points) {
      const float dc_squared{dc * dc};
      cms::alpakatools::for_each_element_in_grid(acc, n_points, [&](uint32_t i) {
        float rho_i{0.f};
        VecArray<float, Ndim> coords_i{dev_points->coords[i]};

        // Get the extremes of the search box
        VecArray<VecArray<float, 2>, Ndim> searchbox_extremes;
        for (int dim{}; dim != Ndim; ++dim) {
          VecArray<float, 2> dim_extremes;
          dim_extremes.push_back(acc, coords_i[dim] - dc);
          dim_extremes.push_back(acc, coords_i[dim] + dc);

          searchbox_extremes.push_back(acc, dim_extremes);
        }

        // Calculate the search box
        VecArray<VecArray<uint32_t, 2>, Ndim> search_box = dev_tiles->searchBox(searchbox_extremes);

        VecArray<uint32_t, Ndim> base_vec;
        for_recursion<TAcc, Ndim, Ndim>(base_vec, search_box, dev_tiles, dev_points, coords_i, &rho_i, dc, i);
      });
    }
  };

  template <typename TAcc, uint8_t Ndim, uint8_t N_>
  ALPAKA_FN_HOST_ACC void for_recursion_nearest_higher(VecArray<uint32_t, Ndim>& base_vec,
                                                       const VecArray<VecArray<uint32_t, 2>, Ndim>& s_box,
                                                       TilesAlpaka<TAcc, Ndim>* tiles,
                                                       PointsView<Ndim>* dev_points,
                                                       float rho_i,
                                                       float* delta_i,
                                                       float* nh_i,
                                                       float dm_sq,
                                                       uint32_t i) {
    if constexpr (N_ == 0) {
      int binId{tiles->getGlobalBinByBin(base_vec)};
      // get the size of this bin
      int binSize{(*tiles)[binId].size()};

      // iterate inside this bin
      for (int binIter{}; binIter < binSize; ++binIter) {
        int j{lt_[binId][binIter]};
        // query N'_{dm}(i)
        float rho_j{dev_points->rho[j]};
        bool found_higher{(rho_j > rho_i)};
        // in the rare case where rho is the same, use detid
        found_higher = found_higher || ((rho_j == rho_i) && (j > i));

        // Calculate the distance between the two points
        VecArray<float, Ndim> j_coords{dev_points[j]};
        float dist_ij_sq{0.f};
        for (int dim{}; dim != Ndim; ++dim) {
          dist_ij_sq += (j_coords[dim] - point_coordinates[dim]) * (j_coords[dim] - point_coordinates[dim]);
        }

        if (found_higher && dist_ij_sq <= dm_sq) {
          // find the nearest point within N'_{dm}(i)
          if (dist_ij_sq < *delta_i) {
            // update delta_i and nearestHigher_i
            *delta_i = dist_ij;
            *nh_i = j;
          }
        }
      }  // end of interate inside this bin

      return;
    } else {
      for (int i{dim_min[dim_min.size() - N_]}; i <= dim_max[dim_max.size() - N_]; ++i) {
        base_vector[base_vector.size() - N_] = i;
        for_recursion_DistanceToHigher<N_ - 1>(
            base_vector, dim_min, dim_max, lt_, rho_i, delta_i, nearestHigher_i, point_id);
      }
    }
  }

  struct KernelCalculateNearestHigher {
    template <typename TAcc, uint8_t Ndim>
    ALPAKA_FN_ACC void operator()(const TAcc& acc,
                                  TilesAlpaka<TAcc, Ndim>* dev_tiles,
                                  PointsView<Ndim>* dev_points,
                                  float outlier_delta_factor,
                                  float dc,
                                  uint32_t n_points) {
      float dm{outlier_delta_factor * dc};
      float dm_squared{dm * dm};
      cms::alpakatools::for_each_element_in_grid(acc, n_points, [&](uint32_t i) {
        float delta_i{std::numeric_limits<float>::max()};
        int nh_i{-1};
        VecArray<float, Ndim> coords_i{dev_points->coords[i]};
        float rho_i{dev_points->rho[i]};

        // Get the extremes of the search box
        VecArray<VecArray<float, 2>, Ndim> searchbox_extremes;
        for (int dim{}; dim != Ndim; ++dim) {
          VecArray<float, 2> dim_extremes;
          dim_extremes.push_back(acc, coords_i[dim] - dc);
          dim_extremes.push_back(acc, coords_i[dim] + dc);

          searchbox_extremes.push_back(acc, dim_extremes);
        }

        // Calculate the search box
        VecArray<VecArray<uint32_t, 2>, Ndim> search_box = dev_tiles->searchBox(searchbox_extremes);

        VecArray<uint32_t, Ndim> base_vec;
        for_recursion_nearest_higher<TAcc, Ndim, Ndim>(
            base_vec, search_box, dev_tiles, dev_points, rho_i, &delta_i, &nh_i, dm_squared, i);

        dev_points->delta[i] = alpaka::math::sqrt(acc, delta_i);
        dev_points->nearest_higher[i] = nh_i;
      });
    }
  };

  struct KernelFindClusters {
    template <typename TAcc, typename TContainer, uint8_t Ndim>
    ALPAKA_FN_ACC void operator()(TAcc const& acc, 
                                  TContainer* followers, 
                                  TContainer* seeds, 
                                  PoitsAlpaka<Ndim>* points, 
                                  uint32_t const& n_points, 
                                  float outlier_delta_factor, 
                                  float d_c, 
                                  float rho_c) const {
      cms::alpakatools::for_each_element_in_grid(acc, n_points, [&](uint32_t i) {
        // initialize cluster_index
        points->cluster_index[i] = -1;

        float delta_i{points->delta[i]};
        float rho_i{points->rho[i]};

        // determine seed or outlier
        bool is_seed{(delta_i > d_c) && (rho_i >= rho_c)};
        bool is_outlier{(delta_i > outlier_delta_factor * d_c) && (rho_i < rho_c)};
        
        // seeds and followers seems to be buffer of VecArray 
        if (is_seed) {
          points->is_seed[i] = 1;
          seeds[0] = i; 
        } else if (!is_outlier) {
          followers[points->nearest_higher[i]][i] = i;
        }

        points->is_seed[i] = 0;
      });
    }
  };

  struct KernelAssignClusters {
    template <typename TAcc, typename TContainer, uint8_t Ndim>
    ALPAKA_FN_ACC void operator()(const TAcc &acc,
                                  TContainer *seeds,
                                  TContainer *followers,
                                  PointsAlpaka<Ndim> *points
                                  uint32_t local_stack_size_per_seed) const {
      const auto &seeds_0 {seeds[0]};
      const auto n_seeds {seeds_0.size()};
      cms::alpakatools::for_each_element_in_grid(acc, n_seeds, [&](uint32_t idx_cls) {
        int local_stack[local_stack_size_per_seed] {-1};
        int local_stack_size {};

        int idx_this_seed {seeds_0[idx_cls]};
        points->cluster_index[idx_this_seed] = idx_cls;
        // push_back idThisSeed to localStack
        // assert((localStackSize < localStackSizePerSeed));
        local_stack[local_stack_size] = idx_this_seed;
        ++local_stack_size;
        // process all elements in localStack
        while (local_stack_size > 0) {
          // get last element of localStack
          // assert((localStackSize - 1 < localStackSizePerSeed));
          int idx_end_of_local_stack {local_stack[local_stack_size - 1]};
          int temp_cluster_index {points->cluster_index[idx_end_of_local_stack]};
          // pop_back last element of localStack
          // assert((localStackSize - 1 < localStackSizePerSeed));
          local_stack[local_stack_size - 1] = -1;
          --local_stack_size;
          const auto &followers_ies {followers[idx_end_of_local_stack]};
          const auto followers_size {followers[idxEndOfLocalStack].size()};
          // loop over followers of last element of localStack
          for (int j {}; j != followers_size; ++j) {
            // pass id to follower
            int follower {followers_ies[j]};
            points->cluster_index[follower] = temp_cluster_index;
            // push_back follower to localStack
            // assert((localStackSize < localStackSizePerSeed));
            local_stack[local_stack_size] = follower;
            ++local_stack_size;
          }
        }
      });
    }
  };
}  // namespace ALPAKA_ACCELERATOR_NAMESPACE
