/**
 * @file
 * This file is part of SeisSol.
 *
 * @author Alex Breuer (breuer AT mytum.de, http://www5.in.tum.de/wiki/index.php/Dipl.-Math._Alexander_Breuer)
 * @author Sebastian Rettenberger (sebastian.rettenberger @ tum.de, http://www5.in.tum.de/wiki/index.php/Sebastian_Rettenberger)
 * 
 * @section LICENSE
 * Copyright (c) 2013-2015, SeisSol Group
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * @section DESCRIPTION
 * Time Step width management in SeisSol.
 **/

#include <atomic>

#include "Parallel/MPI.h"

#include "TimeManager.h"
#include "CommunicationManager.h"
#include <Initializer/preProcessorMacros.fpp>
#include <Initializer/time_stepping/common.hpp>
#include "SeisSol.h"

seissol::time_stepping::TimeManager::TimeManager():
  m_logUpdates(std::numeric_limits<unsigned int>::max())
{
  m_loopStatistics.addRegion("computeLocalIntegration");
  m_loopStatistics.addRegion("computeNeighboringIntegration");
  m_loopStatistics.addRegion("computeDynamicRupture");
  m_loopStatistics.addRegion("stateCorrected");
  m_loopStatistics.addRegion("statePredicted");
  m_loopStatistics.addRegion("stateSynced");

  actorStateStatisticsManager = ActorStateStatisticsManager();
}

seissol::time_stepping::TimeManager::~TimeManager() {
}

void seissol::time_stepping::TimeManager::addClusters(TimeStepping& i_timeStepping,
                                                      MeshStructure* i_meshStructure,
                                                      initializers::MemoryManager& memoryManager,
                                                      bool usePlasticity) {
  SCOREP_USER_REGION( "addClusters", SCOREP_USER_REGION_TYPE_FUNCTION );
  std::vector<std::unique_ptr<GhostTimeCluster>> ghostClusters;
  // assert non-zero pointers
  assert( i_meshStructure         != NULL );

  // store the time stepping
  m_timeStepping = i_timeStepping;

  // iterate over local time clusters
  for (unsigned int localClusterId = 0; localClusterId < m_timeStepping.numberOfLocalClusters; localClusterId++) {
    // get memory layout of this cluster
    auto [meshStructure, globalData] = memoryManager.getMemoryLayout(localClusterId);

    const unsigned int l_globalClusterId = m_timeStepping.clusterIds[localClusterId];
    // chop off at synchronization time
    const auto timeStepSize = m_timeStepping.globalCflTimeStepWidths[l_globalClusterId];
    const long timeStepRate = ipow(static_cast<long>(m_timeStepping.globalTimeStepRates[0]),
         static_cast<long>(l_globalClusterId));

    // Dynamic rupture
    auto& dynRupTree = memoryManager.getDynamicRuptureTree()->child(localClusterId);
    // Note: We need to include the Ghost part, as we need to compute its DR part as well.
    const long numberOfDynRupCells = dynRupTree.child(Interior).getNumberOfCells() +
        dynRupTree.child(Copy).getNumberOfCells() +
        dynRupTree.child(Ghost).getNumberOfCells();

    auto& drScheduler = dynamicRuptureSchedulers.emplace_back(std::make_unique<DynamicRuptureScheduler>(numberOfDynRupCells));

    for (auto type : {Copy, Interior}) {
      const auto offsetMonitoring = type == Interior ? 0 : m_timeStepping.numberOfGlobalClusters;
      // We print progress only if it is the cluster with the largest time step on each rank.
      // This does not mean that it is the largest cluster globally!
      const bool printProgress = (localClusterId == m_timeStepping.numberOfLocalClusters - 1) && (type == Interior);
      clusters.push_back(std::make_unique<TimeCluster>(
          localClusterId,
          l_globalClusterId,
          usePlasticity,
          type,
          timeStepSize,
          timeStepRate,
          printProgress,
          drScheduler.get(),
          globalData,
          &memoryManager.getLtsTree()->child(localClusterId).child(type),
          &dynRupTree.child(Interior),
          &dynRupTree.child(Copy),
          memoryManager.getLts(),
          memoryManager.getDynamicRupture(),
          &m_loopStatistics,
          &actorStateStatisticsManager.addCluster(l_globalClusterId + offsetMonitoring))
      );
    }
    auto& interior = clusters[clusters.size() - 1];
    auto& copy = clusters[clusters.size() - 2];

    // Mark copy layers as higher priority layers.
    interior->setPriority(ActorPriority::Low);
    copy->setPriority(ActorPriority::High);

    // Copy/interior with same timestep are neighbors
    interior->connect(*copy);

    // Connect new copy/interior to previous two copy/interior
    // Then all clusters that are neighboring are connected.
    // Note: Only clusters with a distance of 1 time step factor
    // are connected.
    if (localClusterId > 0) {
      assert(clusters.size() >= 4);
      for (int i = 0; i < 2; ++i)  {
        copy->connect(
            *clusters[clusters.size() - 2 - i - 1]
        );
        interior->connect(
            *clusters[clusters.size() - 2 - i - 1]
        );
      }
    }

#ifdef USE_MPI
    // Create ghost time clusters for MPI
    const int globalClusterId = static_cast<int>(m_timeStepping.clusterIds[localClusterId]);
    for (unsigned int otherGlobalClusterId = 0; otherGlobalClusterId < m_timeStepping.numberOfGlobalClusters; ++otherGlobalClusterId) {
      const bool hasNeighborRegions = std::any_of(meshStructure->neighboringClusters,
                                                  meshStructure->neighboringClusters + meshStructure->numberOfRegions,
                                                  [otherGlobalClusterId](const auto& neighbor) {
        return static_cast<unsigned>(neighbor[1]) == otherGlobalClusterId;
      });
      if (hasNeighborRegions) {
          assert(otherGlobalClusterId >= std::max(globalClusterId - 1, 0));
          assert(otherGlobalClusterId < std::min(globalClusterId +2, static_cast<int>(m_timeStepping.numberOfGlobalClusters)));
        const auto otherTimeStepSize = m_timeStepping.globalCflTimeStepWidths[otherGlobalClusterId];
        const auto otherTimeStepRate = ipow(2l, static_cast<long>(otherGlobalClusterId));

        ghostClusters.push_back(
          std::make_unique<GhostTimeCluster>(
              otherTimeStepSize,
              otherTimeStepRate,
              globalClusterId,
              otherGlobalClusterId,
              meshStructure)
        );
        // Connect with previous copy layer.
        ghostClusters.back()->connect(*copy);
      }
    }
#endif
  }

  // Sort clusters by time step size in increasing order
  auto rateSorter = [](const auto& a, const auto& b) {
    return a->getTimeStepRate() < b->getTimeStepRate();
  };
  std::sort(clusters.begin(), clusters.end(), rateSorter);

  for (const auto& cluster : clusters) {
    if (cluster->getPriority() == ActorPriority::High) {
      highPrioClusters.emplace_back(cluster.get());
    } else {
      lowPrioClusters.emplace_back(cluster.get());
    }
  }

  std::sort(ghostClusters.begin(), ghostClusters.end(), rateSorter);

#ifdef USE_COMM_THREAD
  bool useCommthread = true;
#else
  bool useCommthread = false;
#endif
  if (useCommthread && MPI::mpi.size() == 1)  {
    logInfo(MPI::mpi.rank()) << "Only using one mpi rank. Not using communication thread.";
    useCommthread = false;
  }

  if (useCommthread) {
    communicationManager = std::make_unique<ThreadedCommunicationManager>(std::move(ghostClusters),
                                                                          &seissol::SeisSol::main.getPinning()
                                                                          );
  } else {
    communicationManager = std::make_unique<SerialCommunicationManager>(std::move(ghostClusters));
  }
}

void seissol::time_stepping::TimeManager::scheduleCluster(TimeCluster* cluster) {
  assert(cluster != nullptr);
  cluster->isScheduledAndWaiting = true;
  // Optimization: Empty clusters are scheduled sequentially.
  // TODO(Lukas): Note: This adds a small serial part, maybe schedule at end of loop?
  if (cluster->isEmpty()) {
    logInfo(0) << "Cluster is empty, execute immediately";
    cluster->act();
  } else {
#pragma omp task
    cluster->act();
  }
}

void seissol::time_stepping::TimeManager::advanceInTime(const double &synchronizationTime) {
  SCOREP_USER_REGION( "advanceInTime", SCOREP_USER_REGION_TYPE_FUNCTION )

  // We should always move forward in time
  assert(m_timeStepping.synchronizationTime <= synchronizationTime);

  m_timeStepping.synchronizationTime = synchronizationTime;

  for (auto& cluster : clusters) {
    cluster->setSyncTime(synchronizationTime);
    cluster->reset();
  }

  communicationManager->reset(synchronizationTime);

  seissol::MPI::mpi.barrier(seissol::MPI::mpi.comm());
#ifdef ACL_DEVICE
  device::DeviceInstance &device = device::DeviceInstance::getInstance();
  device.api->putProfilingMark("advanceInTime", device::ProfilingColors::Blue);
#endif

  // Move all clusters from RestartAfterSync to Corrected
  // Does not involve any computations
  for (auto& cluster : clusters) {
    assert(cluster->getNextLegalAction() == ActorAction::RestartAfterSync);
    cluster->act();
    assert(cluster->getState() == ActorState::Corrected);
  }

  bool finished = false; // Is true, once all clusters reached next sync point
#pragma omp parallel
  {
#pragma omp single
    while (!finished) {
      int scheduledTasks = 0;
      finished = true;
      communicationManager->progression();

      // Update all high priority clusters
      std::for_each(highPrioClusters.begin(), highPrioClusters.end(), [&](auto& cluster) {
        if (cluster->isScheduable() &&
            cluster->getNextLegalAction() == ActorAction::Predict) {
          communicationManager->progression();
          scheduledTasks++;
          scheduleCluster(cluster);
        }
      });

      std::for_each(highPrioClusters.begin(), highPrioClusters.end(), [&](auto& cluster) {
        if (cluster->isScheduable() &&
            cluster->getNextLegalAction() != ActorAction::Predict &&
            cluster->getNextLegalAction() != ActorAction::Nothing) {
          communicationManager->progression();
          scheduledTasks++;
          scheduleCluster(cluster);
        }
      });

      // Update one low priority cluster
      if (auto predictable = std::find_if(
            lowPrioClusters.begin(), lowPrioClusters.end(), [](auto& c) {
              return c->isScheduable() &&
                  c->getNextLegalAction() == ActorAction::Predict;
            }
        );
          predictable != lowPrioClusters.end()) {
        scheduledTasks++;
        scheduleCluster(*predictable);
      }
      if (auto correctable = std::find_if(
            lowPrioClusters.begin(), lowPrioClusters.end(), [](auto& c) {
              return c->isScheduable() &&
                  c->getNextLegalAction() != ActorAction::Predict &&
                  c->getNextLegalAction() != ActorAction::Nothing;
            }
        );
          correctable != lowPrioClusters.end()) {
        scheduledTasks++;
        scheduleCluster(*correctable);
      }
      finished = std::all_of(clusters.begin(), clusters.end(),
                             [](auto& c) {
                               return c->synced();
                             });
      finished &= communicationManager->checkIfFinished();

      // Taskwait needed because we otherwise can schedule same cluster twice
      // leading to segfaults. Can be fixed with mutex.
      if (scheduledTasks) {
        logInfo() << "Exit scheduling loop. Scheduled"
        << scheduledTasks << "tasks";
      } else {
        logInfo() << "Exit scheduling loop. Yielding.";
#pragma omp taskwait
      }
    }
  }
#ifdef ACL_DEVICE
  device.api->popLastProfilingMark();
#endif
}

void seissol::time_stepping::TimeManager::printComputationTime()
{
  actorStateStatisticsManager.addToLoopStatistics(m_loopStatistics);
#ifdef USE_MPI
  m_loopStatistics.printSummary(MPI::mpi.comm());
#endif
  m_loopStatistics.writeSamples();
}

double seissol::time_stepping::TimeManager::getTimeTolerance() {
  return 1E-5 * m_timeStepping.globalCflTimeStepWidths[0];
}

void seissol::time_stepping::TimeManager::setPointSourcesForClusters(
    std::unordered_map<LayerType, std::vector<sourceterm::ClusterMapping>>& clusterMappings,
    std::unordered_map<LayerType, std::vector<sourceterm::PointSources>>& pointSources) {
  for (auto& cluster : clusters) {
    cluster->setPointSources(
        clusterMappings[cluster->getLayerType()][cluster->getClusterId()].cellToSources,
        clusterMappings[cluster->getLayerType()][cluster->getClusterId()].numberOfMappings,
        &(pointSources[cluster->getLayerType()][cluster->getClusterId()])
        );
  }
}

void seissol::time_stepping::TimeManager::setReceiverClusters(writer::ReceiverWriter& receiverWriter)
{
  for (auto& cluster : clusters) {
    cluster->setReceiverCluster(receiverWriter.receiverCluster(cluster->getClusterId(),
                                                               cluster->getLayerType()));
  }
}

void seissol::time_stepping::TimeManager::setInitialTimes( double i_time ) {
  assert( i_time >= 0 );

  for(auto & cluster : clusters) {
    cluster->setPredictionTime(i_time);
    cluster->setCorrectionTime(i_time);
    cluster->setReceiverTime(i_time);
  }
}

void seissol::time_stepping::TimeManager::setTv(double tv) {
  for(auto& cluster : clusters) {
    cluster->setTv(tv);
  }
}