/*
 * Copyright [2019] [Ke Sun]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <array>
#include <limits>
#include <random>
#include <conformal_lattice_planner/waypoint_lattice.h>
#include <ros/no_traffic_node.h>

using namespace planner;
using namespace router;

namespace carla {

bool NoTrafficNode::initialize() {

  bool all_param_exist = true;

  // Create publishers.
  map_pub_ = nh_.advertise<visualization_msgs::MarkerArray>("town_map", 1, true);
  traffic_pub_ = nh_.advertise<visualization_msgs::MarkerArray>("traffic", 1, true);

  // Get the world.
  std::string host = "localhost";
  int port = 2000;
  all_param_exist &= nh_.param<std::string>("host", host, "localhost");
  all_param_exist &= nh_.param<int>("port", port, 2000);

  ROS_INFO_NAMED("carla_simulator", "connect to the server.");
  client_ = boost::make_shared<CarlaClient>(host, port);
  client_->SetTimeout(std::chrono::seconds(10));
  world_ = boost::make_shared<CarlaWorld>(client_->GetWorld());
  ros::Duration(1.0).sleep();

  // Applying the world settings.
  double fixed_delta_seconds = 0.05;
  bool no_rendering_mode = true;
  bool synchronous_mode = true;
  all_param_exist &= nh_.param<double>("fixed_delta_seconds", fixed_delta_seconds, 0.05);
  all_param_exist &= nh_.param<bool>("no_rendering_mode", no_rendering_mode, true);
  all_param_exist &= nh_.param<bool>("synchronous_mode", synchronous_mode, true);

  ROS_INFO_NAMED("carla_simulator", "apply world settings.");
  carla::rpc::EpisodeSettings settings = world_->GetSettings();
  if (settings.fixed_delta_seconds) {
    ROS_DEBUG_NAMED("carla_simulator",
        "old settings: fixed_delta_seconds:N/A no_rendering_mode:%d synchronous_mode:%d",
        settings.no_rendering_mode,
        settings.synchronous_mode);
  } else {
    ROS_DEBUG_NAMED("carla_simulator",
        "old settings: fixed_delta_seconds:%f no_rendering_mode:%d synchronous_mode:%d",
        *(settings.fixed_delta_seconds),
        settings.no_rendering_mode,
        settings.synchronous_mode);
  }
  settings.fixed_delta_seconds = fixed_delta_seconds;
  settings.no_rendering_mode = no_rendering_mode;
  settings.synchronous_mode = synchronous_mode;
  world_->ApplySettings(settings);
  ros::Duration(1.0).sleep();

  settings = world_->GetSettings();
  ROS_INFO_NAMED("carla_simulator",
      "new settings: fixed_delta_seconds:%f no_rendering_mode:%d synchronous_mode:%d",
      *(settings.fixed_delta_seconds),
      settings.no_rendering_mode,
      settings.synchronous_mode);

  // Publish the map.
  ROS_INFO_NAMED("carla_simulator", "publish global map.");
  publishMap();

  // Initialize the ego vehicle.
  ROS_INFO_NAMED("carla_simulator", "spawn the vehicles.");
  spawnVehicles();

  // Publish the ego vehicle marker.
  ROS_INFO_NAMED("carla_simulator", "publish ego and agents.");
  publishTraffic();

  // Wait for the planner servers.
  ROS_INFO_NAMED("carla_simulator", "waiting for action servers.");
  ego_client_.waitForServer(ros::Duration(5.0));
  //agents_client_.waitForServer(ros::Duration(5.0));

  // Send out the first goal of ego.
  ROS_INFO_NAMED("carla_simulator", "send the first goals to action servers");
  sendEgoGoal();
  //sendAgentsGoal();

  ROS_INFO_NAMED("carla_simulator", "initialization finishes.");
  return all_param_exist;
}

void NoTrafficNode::spawnVehicles() {

  // The start position.
  // TODO: This may be load from the ROS parameter server.
  std::array<double, 3> start_pt{0, 0, 0};

  // Find the available spawn point cloest to the start point.
  std::vector<CarlaTransform> spawn_points =
    world_->GetMap()->GetRecommendedSpawnPoints();
  CarlaTransform start_transform;
  double min_distance_sq = std::numeric_limits<double>::max();

  for (const auto& pt : spawn_points) {
    const double x_diff = pt.location.x - start_pt[0];
    const double y_diff = pt.location.y - start_pt[1];
    const double z_diff = pt.location.z - start_pt[2];
    const double distance_sq = x_diff*x_diff + y_diff*y_diff + z_diff*z_diff;
    if (distance_sq < min_distance_sq) {
      start_transform = pt;
      min_distance_sq = distance_sq;
    }
  }
  ROS_INFO_NAMED("carla_simulator", "Start waypoint transform\nx:%f y:%f z:%f",
      start_transform.location.x, start_transform.location.y, start_transform.location.z);

  // Start waypoint of the lattice.
  boost::shared_ptr<CarlaWaypoint> start_waypoint =
    world_->GetMap()->GetWaypoint(start_transform.location);

  boost::shared_ptr<WaypointLattice<LoopRouter>> waypoint_lattice=
    boost::make_shared<WaypointLattice<LoopRouter>>(start_waypoint, 100, 1.0, loop_router_);

  // Spawn the ego vehicle.
  // The ego vehicle is at 50m on the lattice, and there is an 100m buffer
  // in the front of the ego vehicle.
  boost::shared_ptr<const CarlaWaypoint> ego_waypoint =
    waypoint_lattice->front(start_waypoint, 50.0)->waypoint();
  if (!ego_waypoint) {
    throw std::runtime_error("Cannot find the ego waypoint on the traffic lattice.");
  }
  ROS_INFO_NAMED("carla_simulator", "Ego vehicle initial transform\nx:%f y:%f z:%f",
      ego_waypoint->GetTransform().location.x,
      ego_waypoint->GetTransform().location.y,
      ego_waypoint->GetTransform().location.z);

  if (!spawnEgoVehicle(ego_waypoint, 25)) {
    throw std::runtime_error("Cannot spawn the ego vehicle.");
  }

  // Let the server know about the vehicles.
  world_->Tick();
  return;
}

boost::optional<size_t> NoTrafficNode::spawnEgoVehicle(
    const boost::shared_ptr<const CarlaWaypoint>& waypoint,
    const double policy_speed,
    const bool noisy_policy_speed,
    const bool noisy_start_speed) {

  // Get the blueprint of the ego vehicle.
  boost::shared_ptr<CarlaBlueprintLibrary> blueprint_library =
    world_->GetBlueprintLibrary()->Filter("vehicle");
  auto blueprint = blueprint_library->at("vehicle.audi.tt");

  // Make sure the vehicle will fall onto the ground instead of fall endlessly.
  CarlaTransform transform = waypoint->GetTransform();
  transform.location.z += 1.5;

  boost::shared_ptr<CarlaActor> actor = world_->TrySpawnActor(blueprint, transform);
  boost::shared_ptr<CarlaVehicle> vehicle = boost::static_pointer_cast<CarlaVehicle>(actor);

  if (!actor) {
    // Cannot spawn the actor.
    // There might be a collision at the waypoint or something.
    ROS_ERROR_NAMED("carla simulator", "Cannot spawn the ego vehicle.");
    return boost::none;
  }

  // Set the ego vehicle policy.
  const size_t seed = std::chrono::system_clock::now().time_since_epoch().count();
  std::default_random_engine rand_gen(seed);
  std::uniform_real_distribution<double> uni_real_dist(-2.0, 2.0);

  if (noisy_policy_speed) {
    ego_policy_.first = vehicle->GetId();
    ego_policy_.second = policy_speed + uni_real_dist(rand_gen);
  } else {
    ego_policy_.first = vehicle->GetId();
    ego_policy_.second = policy_speed;
  }

  if (noisy_start_speed) {
    vehicle->SetVelocity(transform.GetForwardVector() *
                         (ego_policy_.second + uni_real_dist(rand_gen)));
  } else {
    vehicle->SetVelocity(transform.GetForwardVector() *
                         ego_policy_.second);
  }

  return vehicle->GetId();
}
} // End namespace carla.