/* Change log:
 * 1. Ability to takeoff at any position
 * 2. Subscribed to RC disconnect status
 *
 *
 *
 *
 */


#include <glog/logging.h>
#include <thread>
#include <mutex>
#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/offboard/offboard.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <chrono>
#include <atomic>
#include <mosquitto.h>
#include <cstring>


enum command{
	ARM = 10,
	LAND = 20,
	HOLD = 30,
	INITIAL_SETPOINT = 40,
	OFFBOARD_START = 50,
	OFFBOARD_STOP = 60,
	TAKEOFF = 70,
	DISARM = 80,
	POSITION = 90,
	VELOCITY = 100,
	ESTABLISH_CONNECTION = 110,
	TARGET = 120,
	MOVE = 130,
	DESCEND = 140,
	ACCEL_EAST = 150,
	DECEL_EAST = 160,
	ACCEL_NORTH = 170,
	DECEL_NORTH = 180
};


class message {
	public:
		uint64_t timestamp;
		int oper;
		float field1;
		float field2;
		float field3;
		float field4;
		float field5;
		float field6;
};

float target_north, target_east, target_down, \
	      cur_north, cur_east, cur_down;

std::mutex payload_mutex;
std::mutex position_mutex;
message payload;
std::atomic<bool> close_connection(false); 
std::atomic<bool> comm_fail(true);
std::atomic<bool> mission_started(false);
std::atomic<bool> mission_complete(false);
//std::atomic<int> cur_flight_mode(command::DISARM);

const int max_comm_blackout = 5;
const float alt_err_margin = 0.4;
const float pos_err_margin = 0.4;
const float ned_err_margin = 0.7;
const int num_drones = 4;


void deserialize_message(const uint8_t* buffer, message& msg) {
	std::memcpy(&msg.timestamp, buffer, sizeof(msg.timestamp));
	std::memcpy(&msg.oper, buffer + sizeof(msg.timestamp), sizeof(msg.oper));
	std::memcpy(&msg.field1, buffer + sizeof(msg.timestamp) + sizeof(msg.oper), sizeof(msg.field1));
	std::memcpy(&msg.field2, buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1), sizeof(msg.field2));
	std::memcpy(&msg.field3, buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1) + sizeof(msg.field2), sizeof(msg.field3));
	std::memcpy(&msg.field4, buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1) + sizeof(msg.field2) + sizeof(msg.field3), sizeof(msg.field4));
	std::memcpy(&msg.field5, buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1) + sizeof(msg.field2) + sizeof(msg.field3) + sizeof(msg.field4), sizeof(msg.field5));
	std::memcpy(&msg.field6, buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1) + sizeof(msg.field2) + sizeof(msg.field3) + sizeof(msg.field4) + sizeof(msg.field5), sizeof(msg.field6));
}


void on_connect(struct mosquitto* mosq, void* userdata, int result) {
	if (result == MOSQ_ERR_SUCCESS) {
		LOG(INFO) << "Connected to broker! Subscribing to topic...\n";

		int ret = mosquitto_subscribe(mosq, NULL, "node1/status", 1);
		if (ret != MOSQ_ERR_SUCCESS )
			LOG(INFO) << "Failed to subscribe to topic node1/status ...\n";

		ret = mosquitto_subscribe(mosq, NULL, "$SYS/broker/clients/connected", 0);
		if (ret != MOSQ_ERR_SUCCESS)
			LOG(INFO) << "Failed to subscribe to topic $SYS/broker/clients/connected ...\n";
	} 
	else {
		LOG(INFO) << "Failed to connect: " << mosquitto_strerror(result) << "\n";
		comm_fail.store(true);
	}
}


void on_disconnect(struct mosquitto *mosq, void* userdata, int result){

	if(result == 0){
		LOG(INFO) << "Client called mosquitto disconnect. \n";
	}
	else {
		LOG(INFO) << "Disconnnected from broker unexpectedly with result code: " << result << "\n";
		//comm_fail.store(true);
	}

}


void on_message(struct mosquitto* mosq, void* userdata, const struct mosquitto_message* message) {
	if (!strcmp(message->topic, "$SYS/broker/clients/connected")){
		int connected_clients = atoi(static_cast<const char*>(message->payload));
		if (connected_clients < num_drones) {
			//mission in progress
			if (mission_started.load() && !mission_complete.load()){
				LOG(INFO) << "Clients disconnected during mission.\n";
				comm_fail.store(true);
			}
			else if (!mission_started.load() && !mission_complete.load()){
				LOG(INFO) << "Waiting for all drones to connect to start mission...\n";
			}
			else if (mission_started.load() && mission_complete.load()){
				LOG(INFO) << "Mission completed clients disconnected\n";	
			}
		}
		else if (connected_clients == num_drones){
			comm_fail.store(false);
			mission_started.store(true);
		}
		LOG(INFO) << connected_clients << " drones connected...\n";
	}
	
	else if (!strcmp(message->topic, "node1/status")){
		if (message->payloadlen == 36){
			std::lock_guard<std::mutex> lock(payload_mutex);
			deserialize_message(static_cast<uint8_t*>(message->payload), payload);
		}
		else {
			LOG(INFO) << "Received message with unexpected size...\n";
		}
	}
}


int comm_module(){

	mosquitto_lib_init();

	struct mosquitto* mosq = mosquitto_new("node2", true, NULL);
	if (!mosq){
		LOG(INFO) << "Failed to create Mosquitto client...\n";
		return -1;
	}

	mosquitto_connect_callback_set(mosq, on_connect);
	mosquitto_message_callback_set(mosq, on_message);
	mosquitto_disconnect_callback_set(mosq, on_disconnect);

	int rc = mosquitto_connect(mosq, "192.168.4.1", 1883, 60);
	//int rc = mosquitto_connect(mosq, "localhost", 1883, 60);
	if (rc != MOSQ_ERR_SUCCESS){
		LOG(INFO) << "Failed to connect to broker: " << mosquitto_strerror(rc) << "\n";
		mosquitto_destroy(mosq);
		mosquitto_lib_cleanup();
		return -1;
	}

	while(!close_connection){
		mosquitto_loop(mosq, 100, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
	LOG(INFO) << "Communication thread completed...\n";

	return 0;
}


void emergency_hold(mavsdk::Action &action){
	close_connection.store(true);
	action.hold();
	LOG(INFO) << "Quadcopter on hold.....\n";
	std::this_thread::sleep_for(std::chrono::seconds(2));
	const auto land_result = action.land();
	if (land_result != mavsdk::Action::Result::Success){
		LOG(INFO) << "Landing failed..." << land_result << "\n";
	}
}


void position_update(mavsdk::Telemetry::PositionVelocityNed pos){
	std::lock_guard<std::mutex> lock_pos(position_mutex);
	cur_north = pos.position.north_m;
	cur_east = pos.position.east_m;
	cur_down = pos.position.down_m;
}
/*
void rc_status_update(mavsdk::Telemetry::RcStatus rcS){
	//LOG(INFO) << "Radio status: " << rcS.was_available_once << rcS.is_available << rcS.signal_strength_percent << "\n";
}
*/

int main(){

	google::SetLogDestination(google::INFO, "/home/ubuntu2/drone/logs/receiver2.log");
	FLAGS_alsologtostderr = 1;
	google::InitGoogleLogging("Receiver2");

	mavsdk::Mavsdk::Configuration config{mavsdk::Mavsdk::ComponentType::CompanionComputer};
	config.set_always_send_heartbeats(true);
	mavsdk::Mavsdk mavsdk{config};
	//mavsdk::ConnectionResult connection_result = mavsdk.add_any_connection("udp://0.0.0.0:14541");
	mavsdk::ConnectionResult connection_result = mavsdk.add_any_connection("serial:///dev/serial0:115200");
	
	if(connection_result != mavsdk::ConnectionResult::Success){
		LOG(INFO) << "Connection failed: " << connection_result << "\n";
		return -1;
	}


	auto system = mavsdk.first_autopilot(3.0);
	if(!system){
	 	LOG(INFO) << "Timed out waiting for system \n";
	 	return -1;
	 }


	 auto action = mavsdk::Action{system.value()};
	 auto offboard = mavsdk::Offboard{system.value()};
	 auto telemetry = mavsdk::Telemetry{system.value()};

	//auto sys = mavsdk.systems();
	// LOG(INFO) << (*system)->get_system_id();

	// LOG(INFO) << sys[0] << " " << sys[1];
	//auto action = mavsdk::Action{mavsdk.systems()[1]};
	//auto offboard = mavsdk::Offboard{mavsdk.systems()[1]};
	//auto telemetry = mavsdk::Telemetry{mavsdk.systems()[1]};

	while(!telemetry.health_all_ok()){
		LOG(INFO) << "Waiting for system to be ready... \n";
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	LOG(INFO) << "System is ready \n";

	telemetry.set_rate_position_velocity_ned(80);
	//telemetry.set_rate_rc_status(1);
	mavsdk::Telemetry::PositionVelocityNedHandle pos_handle = telemetry.subscribe_position_velocity_ned(position_update);
	//mavsdk::Telemetry::RcStatusHandle rcs_handle = telemetry.subscribe_rc_status(rc_status_update);
/*
	bool ned_status = false;
	while(!ned_status){
		std::this_thread::sleep_for(std::chrono::seconds(1));
		{
			std::lock_guard<std::mutex> lock_pos(position_mutex);
			if (abs(cur_down) <= ned_err_margin) 
				ned_status = true;
			else
				LOG(INFO) << "NED greater than: " << ned_err_margin << "\n";
		}
	}
*/
	LOG(INFO) << "Starting communication thread...\n";
	std::thread comm_thread(comm_module);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));


	while(comm_fail.load()){
		//LOG(INFO) << "Waiting...\n";
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	message payload_l;
	int cur_flight_mode = command::DISARM;
	float start_north = cur_north;
	float start_east = cur_east;
	bool target_received = false;
	LOG(INFO) << "Starting position: " << start_north << ";" << start_east << ";" << cur_down << "\n";
	while(!comm_fail.load()){	
		{
		std::lock_guard<std::mutex> lock(payload_mutex);
		payload_l.oper = payload.oper;
		payload_l.field1 = payload.field1;
		payload_l.field2 = payload.field2;
		payload_l.field3 = payload.field3;
		payload_l.field4 = payload.field4;
		payload_l.field5 = payload.field5;
		payload_l.field6 = payload.field6;
		}

		float cur_north_l, cur_east_l, cur_down_l;
		{
		std::lock_guard<std::mutex> lock_pos(position_mutex);
		cur_north_l = cur_north;
		cur_east_l = cur_east;
		cur_down_l = cur_down;
		}


		//check for valid state change
		if (payload_l.oper == command::ARM && (cur_flight_mode == command::DISARM)){
			LOG(INFO) << "ARM";
			const auto arm_result = action.arm();
			if(arm_result != mavsdk::Action::Result::Success){
				LOG(INFO) << "Arming failed" << arm_result << "\n";
				emergency_hold(action);
				break;
			}
			LOG(INFO) << "Armed \n";
			cur_flight_mode = command::ARM;
		}
		else if (payload_l.oper == command::TARGET ){//&& !target_received){
			if (payload_l.field1 != target_north || payload_l.field2 != target_east \
				|| payload_l.field3 != target_down){
				target_north = payload_l.field1;
				target_east = payload_l.field2;
				target_down = payload_l.field3;
				LOG(INFO) << "Target received: " << target_north << ", " << target_east << ", " << target_down << "\n";
			}
		}
		else if (payload_l.oper == command::LAND && cur_flight_mode != command::LAND){ //land can be activated in 
			const auto land_result = action.land();
			if (land_result != mavsdk::Action::Result::Success){
				LOG(INFO) << "Landing failed..." << land_result << "\n";
				emergency_hold(action);
				break;
			}
			LOG(INFO) << "Landing...\n";
			cur_flight_mode = command::LAND;
			mission_complete.store(true);

			while(telemetry.landed_state() != mavsdk::Telemetry::LandedState::OnGround){
				;//wait till landed
			}

			//auto disarm_result = action.disarm();
			//if (disarm_result != mavsdk::Action::Result::Success){
			//	LOG(INFO) << "Disarming failed" << disarm_result << "\n";
			//	break;
			//}

			//auto hold_result = action.hold(); //updating the state of the sytem
			break;
		}
		else if (payload_l.oper == command::INITIAL_SETPOINT && cur_flight_mode == command::ARM){
			LOG(INFO) << "Initial setpoint set";
			const mavsdk::Offboard::VelocityNedYaw stay{};
			offboard.set_velocity_ned(stay);
			cur_flight_mode = command::INITIAL_SETPOINT;
		}
		else if (payload_l.oper == command::OFFBOARD_START && cur_flight_mode == command::INITIAL_SETPOINT){
			const mavsdk::Offboard::VelocityNedYaw stay{};
			offboard.set_velocity_ned(stay);
			const auto offboard_start_result = offboard.start();
			if(offboard_start_result != mavsdk::Offboard::Result::Success){
				LOG(INFO) << "Offboard start failed: " << offboard_start_result << "\n";
				emergency_hold(action);
				break;
			}
			LOG(INFO) << "Offboard started \n";
			cur_flight_mode = command::OFFBOARD_START;
		}
		else if (payload_l.oper == command::TAKEOFF && cur_flight_mode == command::OFFBOARD_START){
			offboard.set_position_ned((mavsdk::Offboard::PositionNedYaw){start_north, start_east, target_down, 0});
			LOG(INFO) << "Goto target altitude...\n";
			/*bool target = false;
			while(!target && !comm_fail.load()){
				{
					std::lock_guard<std::mutex> lock_pos(position_mutex);
					if (target_down - cur_down > -1*alt_err_margin){
						target = true;
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}

			if (target && !comm_fail.load())
				LOG(INFO) << "Target altitude reached...\n";
			else
				emergency_hold(action);
			*/

			cur_flight_mode = command::TAKEOFF;
		}
		else if (payload_l.oper == command::HOLD && cur_flight_mode != command::HOLD){
			const auto hold_result = action.hold();
			if(hold_result != mavsdk::Action::Result::Success){
				LOG(INFO) << "Hold failed..." << hold_result << "\n";
				break;
			}
			cur_flight_mode = command::HOLD;
		}
		else if(payload_l.oper == command::VELOCITY && (cur_flight_mode == command::OFFBOARD_START || cur_flight_mode == command::TAKEOFF)){
			//LOG(INFO) << payload_l.field4 << "," << payload_l.field5 << "," << payload_l.field6 << "\n";
			//TODO: Direction guaranteed saturation needs to be implemented for velocity control
			if (abs(payload_l.field4) < 2 || abs(payload_l.field5) < 2 || abs(payload_l.field6) < 2){
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){payload_l.field4, payload_l.field5, payload_l.field6, 0});
			}
		}
		else if (payload_l.oper == command::MOVE && (cur_flight_mode == command::OFFBOARD_START || cur_flight_mode == command::TAKEOFF)){
			//TODO::
			offboard.set_position_ned((mavsdk::Offboard::PositionNedYaw){payload_l.field1, payload_l.field2, payload_l.field3, 0});
		}
		else if (payload_l.oper == command::POSITION && (cur_flight_mode == command::OFFBOARD_START || cur_flight_mode == command::TAKEOFF)){
			//TODO: Need to handle this
			offboard.set_position_ned((mavsdk::Offboard::PositionNedYaw){payload_l.field1, payload_l.field2, payload_l.field3, 0});
		}
		else if(payload_l.oper == command::DISARM && cur_flight_mode == command::LAND){
			const auto disarm_result = action.disarm();
			LOG(INFO) << "Disarmed...\n";
			cur_flight_mode = command::DISARM;
			break;
		}
		else if (payload_l.oper == command::OFFBOARD_STOP && (cur_flight_mode == command::OFFBOARD_START || cur_flight_mode == command::TAKEOFF)){
			const auto offboard_stop_result = offboard.stop();
			LOG(INFO) << "Offboard stop...\n";
			if(offboard_stop_result != mavsdk::Offboard::Result::Success){
				LOG(INFO) << "Offboard stop failed: " << offboard_stop_result << "\n";
				emergency_hold(action);
			}
			cur_flight_mode = command::OFFBOARD_STOP;
		}
		else if (payload_l.oper == command::ACCEL_EAST && (cur_flight_mode == command::OFFBOARD_START || cur_flight_mode == command::TAKEOFF)){
			float vec_n = target_north - cur_north_l;
			float vec_e = target_east - cur_east_l;
			float vec_d = target_down - cur_down_l;
			float vec_res = sqrt(pow(vec_n,2)+pow(vec_e,2)+pow(vec_d,2));
			offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){vec_n/vec_res, payload_l.field1*vec_e/vec_res, vec_d/vec_res, 0});		
		}
		else if (payload_l.oper == command::DECEL_EAST && (cur_flight_mode == command::OFFBOARD_START || cur_flight_mode == command::TAKEOFF)){
			float vec_n = target_north - cur_north_l;
			float vec_e = target_east - cur_east_l;
			float vec_d = target_down - cur_down_l;
			if (abs(vec_e) < pos_err_margin && abs(vec_d) < alt_err_margin){
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){0, 0, 0, 0});
			}
			else {
				float vec_res = sqrt(pow(vec_n,2)+pow(vec_e,2)+pow(vec_d,2));
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){vec_n/vec_res, payload_l.field1*vec_e/vec_res, vec_d/vec_res, 0});
			}
		}
		else if (payload_l.oper == command::ACCEL_NORTH && (cur_flight_mode == command::OFFBOARD_START || cur_flight_mode == command::TAKEOFF)){
			float vec_n = target_north - cur_north_l;
			float vec_e = target_east - cur_east_l;
			float vec_d = target_down - cur_down_l;
			float vec_res = sqrt(pow(vec_n,2)+pow(vec_e,2)+pow(vec_d,2));
			offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){payload_l.field1*vec_n/vec_res, vec_e/vec_res, vec_d/vec_res, 0});		
		}
		else if (payload_l.oper == command::DECEL_NORTH && (cur_flight_mode == command::OFFBOARD_START || cur_flight_mode == command::TAKEOFF)){
			float vec_n = target_north - cur_north_l;
			float vec_e = target_east - cur_east_l;
			float vec_d = target_down - cur_down_l;
			if(abs(vec_n) < pos_err_margin && abs(vec_d) < alt_err_margin){
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){0, 0, 0, 0});
			}
			else{
				float vec_res = sqrt(pow(vec_n,2)+pow(vec_e,2)+pow(vec_d,2));
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){payload_l.field1*vec_n/vec_res, vec_e/vec_res, vec_d/vec_res, 0});
			}
		}
	}

	//while loop exited due to loosing heartbeat
	if(comm_fail.load()){
		emergency_hold(action);
	}
	
	close_connection.store(true);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	comm_thread.join();
	while(telemetry.landed_state() != mavsdk::Telemetry::LandedState::OnGround){
		;//wait till landed
	}

	std::this_thread::sleep_for(std::chrono::seconds(4));

	if(telemetry.flight_mode() == mavsdk::Telemetry::FlightMode::Offboard){
		const auto offboard_stop_result = offboard.stop();
		if (offboard_stop_result != mavsdk::Offboard::Result::Success){
			LOG(INFO) << "Offboard stop failed: " << offboard_stop_result << "\n";
		}
	}
	return 1;

}
