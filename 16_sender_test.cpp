/*Version Note: 
 * Previous 15_sender_test. Changes from previous version:
 * Generalized the position control for moving in target direction
 * Need to change the parameter of the drone MPC_XY_VEL_MAX and MPC_Z_VEL_MAX_DN
 *
 *
 *
 * */


#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/offboard/offboard.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <glog/logging.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>

extern "C" {
    #include <mosquitto.h>
}

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


message payload;
message position;
std::mutex payload_mutex;
std::mutex position_mutex;

//std::atomic<bool> comm_thread_done(false);
std::atomic<bool> main_thread_done(false);
std::atomic<bool> comm_fail(true);
std::atomic<bool> pos_update(false);
std::atomic<bool> mission_started(false);
std::atomic<bool> mission_complete(false);

const int max_comm_blackout = 5; // maximum time a follower does not communicate with leader before starting emergency procedure
const float alt_err_margin = 0.3; //metres
const float pos_err_margin = 0.3; //metres 
const int max_alt = 70; //
const int num_drones = 4;

//mqtt variables
uint8_t buffer[36];


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


void serialize_message(const message& msg, uint8_t* buffer) {
	// Copy each field into the buffer
	// msg in this is reference. Normally if we wish to access operation we can do msg.operation but we need the address.Hence we use the & operator
	memcpy(buffer, &msg.timestamp, sizeof(msg.timestamp));
	memcpy(buffer + sizeof(msg.timestamp), &msg.oper, sizeof(msg.oper));
	memcpy(buffer + sizeof(msg.timestamp) + sizeof(msg.oper), &msg.field1, sizeof(msg.field1));
	memcpy(buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1), &msg.field2, sizeof(msg.field2));
	memcpy(buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1) + sizeof(msg.field2), &msg.field3, sizeof(msg.field3));
	memcpy(buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1) + sizeof(msg.field2) + sizeof(msg.field3), &msg.field4, sizeof(msg.field4));
	memcpy(buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1) + sizeof(msg.field2) + sizeof(msg.field3) + sizeof(msg.field4), &msg.field5, sizeof(msg.field5));
	memcpy(buffer + sizeof(msg.timestamp) + sizeof(msg.oper) + sizeof(msg.field1) + sizeof(msg.field2) + sizeof(msg.field3) + sizeof(msg.field4) + sizeof(msg.field5), &msg.field6, sizeof(msg.field6));
}


void on_sys_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
	if (strcmp(msg->topic, "$SYS/broker/clients/connected") == 0) {
		int connected_clients = atoi(static_cast<const char*>(msg->payload));
		if(connected_clients < num_drones){
			if (mission_started.load() && !mission_complete.load()){
				LOG(INFO) << "Clients disconnected during mission. Starting emergency procedures...\n";
				comm_fail.store(true);
			}
			else if(!mission_started.load() && !mission_complete.load()){
				LOG(INFO) << "Waiting for all drones to connect to start mission...\n";
			}
			else if(mission_started.load() && mission_complete.load()){
				LOG(INFO) << "Mission completed clients disconnected\n";
			}
		}
		else if (connected_clients == num_drones){
			LOG(INFO) << "All clients connected starting the mission...\n";
			comm_fail.store(false);
			mission_started.store(true);
		}
	}
}


void on_disconnect(struct mosquitto *mosq, void *obj, int result){
	
	if (result == 0){
		LOG(INFO) << "Client called mosquitto disconnect...\n";
	}
	else {
		LOG(INFO) << "Disconnected from broker unexpectedly with result code: " << result << "\n";
		//comm_fail.store(true);
	}

}


int comm_module(){
	mosquitto *mosq = mosquitto_new("node1", true, nullptr);
	if(!mosq){
		LOG(INFO) << "Failed to create Mosquitto client.\n";
		return -1;
	}

	int ret = mosquitto_connect(mosq, "localhost", 1883, 60);
	if(ret != MOSQ_ERR_SUCCESS){
		LOG(INFO) << "Unable to connect: " << mosquitto_strerror(ret) << "\n";
		mosquitto_destroy(mosq);
		mosquitto_lib_cleanup();
		return -1;
	}
	LOG(INFO) << "Successfully connected to broker...\n";
	
	mosquitto_message_callback_set(mosq, on_sys_message);
	mosquitto_disconnect_callback_set(mosq, on_disconnect);
	
	int sys_ret = mosquitto_subscribe(mosq, nullptr, "$SYS/broker/clients/connected", 0);
	
	if(sys_ret != MOSQ_ERR_SUCCESS){
		LOG(INFO) << "Unable to subscribe to system status" << mosquitto_strerror(sys_ret) << "\n";
		mosquitto_destroy(mosq);
		mosquitto_lib_cleanup();
		return -1;
	}
	else
		LOG(INFO) << "Subscribed to the broker/clients/connected message...\n";


	const char* topic = "node1/status";
	while(!main_thread_done.load()){
		{
			std::lock_guard<std::mutex> lock(payload_mutex);
			serialize_message(payload, buffer);
		}
		ret = mosquitto_publish(mosq, nullptr, topic, sizeof(buffer), buffer, 1, false);
		if(ret != MOSQ_ERR_SUCCESS){
			LOG(INFO) << "Failed to publish: " << mosquitto_strerror(ret) << "\n";
		}
		mosquitto_loop(mosq, 100, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	
	mosquitto_disconnect(mosq);
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
	LOG(INFO) << "Communication thread completed\n";
	return 1;
}


void update_payload(int operation, float field1=0, float field2=0, float field3=0, float field4=0, float field5=0, float field6=0){
	std::lock_guard<std::mutex> lock(payload_mutex);
	payload.timestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
	payload.oper = operation;
	payload.field1 = field1;
	payload.field2 = field2;
	payload.field3 = field3;
	payload.field4 = field4;
	payload.field5 = field5;
	payload.field6 = field6;
}


//callback function for sensor update
void position_update(mavsdk::Telemetry::PositionVelocityNed pos){
    /*
    if (pos_update.load()){
	//LOG(INFO) << pos.velocity.down_m_s;
    	update_payload(command::POSITION, pos.position.north_m, pos.position.east_m, pos.position.down_m, pos.velocity.north_m_s, pos.velocity.east_m_s, pos.velocity.down_m_s);
    }*/

    std::lock_guard<std::mutex> lock_pos(position_mutex);
    position.oper = command::POSITION;
    position.field1 = pos.position.north_m;
    position.field2 = pos.position.east_m;
    position.field3 = pos.position.down_m;
}


void flight_mode_update(mavsdk::Telemetry::FlightMode flight_mode){
	if (flight_mode == mavsdk::Telemetry::FlightMode::Land){
		update_payload(command::LAND);
	}
	else if (flight_mode == mavsdk::Telemetry::FlightMode::Hold){
		update_payload(command::HOLD);
	}
}


int emergency_hold(mavsdk::Action &action, std::thread &comm_thread, mavsdk::Telemetry &telemetry, mavsdk::Telemetry::PositionVelocityNedHandle &pos_handle){
	//perform emergency hold
	
	update_payload(command::HOLD);

	auto hold_result = action.hold();
	if (hold_result != mavsdk::Action::Result::Success){
		main_thread_done.store(true);
		LOG(INFO) << "Unable to perform emergency hold: " << hold_result << "\n";
		return -1;
	}

	LOG(INFO) << "Emergency hold \n";

	std::this_thread::sleep_for(std::chrono::seconds(2));
	auto land_result = action.land();
	if(land_result != mavsdk::Action::Result::Success){
		LOG(INFO) << "Emergency land failed..." << land_result << "\n";
	}
	main_thread_done.store(true);

	std::this_thread::sleep_for(std::chrono::seconds(2));
	comm_thread.join();
	telemetry.unsubscribe_position_velocity_ned(pos_handle);

	return 1;
}


int move_north(mavsdk::Offboard &offboard, float target_north, float target_east, float target_down){
	LOG(INFO) << "Moving in the North direction\n";
		float i = 0.f;//north
		bool target = false;
		float north_increment = 8;
		float north_next_point = 6;
		int north_quad = 1;
		float cur_north, cur_east, cur_down;
		float acc = 0.f;
		float dcc = 6.f;
		bool ret = false;
		bool first = true;
		float decel_distance = 20.f;

		{
			std::lock_guard<std::mutex> lock_pos(position_mutex);
			cur_north = position.field1;
			cur_east = position.field2;
			cur_down = position.field3;
		}

		float north_distance = abs(abs(target_north) - abs(cur_north));

		abs(target_north) > abs(cur_north) ? ret = false : ret = true;

		if (ret == true){
			north_increment = north_increment*-1;
			north_next_point = north_next_point*-1;
		}

		if (target_north < 0)
			north_quad = -1;

		while(!target && !comm_fail.load()){
			{
				std::lock_guard<std::mutex> lock_pos(position_mutex);
				cur_north = position.field1;
				cur_east = position.field2;
				cur_down = position.field3;
			}

			if (abs(target_down - cur_down) < alt_err_margin && abs(target_north - cur_north) < pos_err_margin){
				update_payload(command::VELOCITY, 0, 0, 0, 0, 0, 0);
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){0, 0, 0, 0});
				target = true;
			}
			//else if ((abs(cur_north) <= abs(target_north)*0.25 && ret == false) || (abs(cur_north) >= abs(target_north)*0.25 && ret == true)){
			/*
			else if (abs(abs(target_north)- abs(cur_north)) >= north_distance*0.75){
				if(acc+0.5<=6)
					acc = acc+0.5;
				else
					acc = 6;

				float vec_n = target_north - cur_north;
				float vec_e = target_east - cur_east;
				float vec_d = target_down - cur_down;
				float vec_res = sqrt(pow(vec_n,2)+pow(vec_e,2)+pow(vec_d,2));
				update_payload(command::ACCEL_NORTH, acc);
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){acc*vec_n/vec_res, vec_e/vec_res, vec_d/vec_res, 0});
				std::this_thread::sleep_for(std::chrono::milliseconds(150));
				LOG(INFO) << acc;
			}*/
			//deceleration stage 
			//else if ((abs(cur_north) >= abs(target_north)*0.80 && ret == false) || (abs(cur_north) <= abs(target_north)*0.80 && ret == true)){
			//else if (abs(abs(target_north)- abs(cur_north)) <= north_distance*0.25){
			else if (abs(abs(target_north) - abs(cur_north)) <= decel_distance){
				if(dcc-0.5>=0.5)
					dcc = dcc-0.4;
				else
					dcc = 0.5;

				float vec_n = target_north - cur_north;
				float vec_e = target_east - cur_east;
				float vec_d = target_down - cur_down;
				float vec_res = sqrt(pow(vec_n,2)+pow(vec_e,2)+pow(vec_d,2));
				update_payload(command::DECEL_NORTH, dcc);
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){dcc*vec_n/vec_res, vec_e/vec_res, vec_d/vec_res, 0});
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				LOG(INFO) << dcc;
			}
			else {
				while(first){
					i = abs(cur_north) + north_increment;
					first = false;
					LOG(INFO) << "Inside position phase...";
				}
				switch (ret){
					case 0:
						if ((abs(cur_north) >= i - north_next_point) && (abs(cur_north) + north_increment <= abs(target_north*0.75))) {
							i = i + north_increment;
						} 
						else if (abs(cur_north) + north_increment > abs(target_north)){
							i = abs(target_north);
						}
						break;
					case 1:
						if ((abs(cur_north) <= i - north_next_point) && (abs(cur_north) + north_increment >= abs(target_north*0.75))) {
							i = i + north_increment;
						} 
						else if (abs(cur_north) + north_increment < abs(target_north)){
							i = abs(target_north);
						}
						break;					
				}
				LOG(INFO) << i;
				update_payload(command::POSITION, north_quad*i, 0, target_down, 0, 0, 0);
				offboard.set_position_ned((mavsdk::Offboard::PositionNedYaw){north_quad*i, target_east, target_down, 0});
			}
		}

		if (target && !comm_fail.load()){
			LOG(INFO) << "Target North position reached\n";
			return 0;
		}
		else if (comm_fail.load()){
			LOG(INFO) << "Communication failed. Starting emergency hold. \n";
			return -1;
		}
		return 0;

}

int move_east(mavsdk::Offboard &offboard, float target_north, float target_east, float target_down){
	LOG(INFO) << "Moving in the East direction of target\n";

		float j = 0.f;//east
		bool target = false;
		float east_increment = 8;
		float east_next_point = 6;
		int east_quad = 1;
		float cur_north, cur_east, cur_down;
		float acc = 0.f;
		float dcc = 6.f;
		bool ret = false;
		bool first = true;
		float decel_distance = 20.f;

		{
			std::lock_guard<std::mutex> lock_pos(position_mutex);
			cur_north = position.field1;
			cur_east = position.field2;
			cur_down = position.field3;
		}

		float east_distance = abs(abs(target_east) - abs(cur_east));
		abs(target_east) > abs(cur_east) ? ret = false : ret = true;

		if (ret == true){
			east_increment = east_increment*-1;
			east_next_point = east_next_point*-1;
		}

		if (target_east < 0)
			east_quad = -1;

		while(!target && !comm_fail.load()){
			{
				std::lock_guard<std::mutex> lock_pos(position_mutex);
				cur_north = position.field1;
				cur_east = position.field2;
				cur_down = position.field3;
			}

			if (abs(target_down - cur_down) < alt_err_margin && abs(target_north - cur_north) < pos_err_margin && abs(target_east - cur_east) < pos_err_margin){
				target = true;
				update_payload(command::VELOCITY, 0, 0, 0, 0, 0, 0);
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){0, 0, 0, 0});
			}
			//acceleration stage
			//else if ((abs(cur_east) <= abs(target_east)*0.25 && ret == false) || (abs(cur_east) >= abs(target_east)*0.25 && ret == true)){
			/*
			else if (abs(abs(target_east)- abs(cur_east)) >= east_distance*0.75){
				if(acc+0.5<=6)
					acc = acc+0.5;
				else
					acc = 6;

				float vec_n = target_north - cur_north;
				float vec_e = target_east - cur_east;
				float vec_d = target_down - cur_down;
				float vec_res = sqrt(pow(vec_n,2)+pow(vec_e,2)+pow(vec_d,2));
				update_payload(command::ACCEL_EAST, acc);
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){vec_n/vec_res, acc*vec_e/vec_res, vec_d/vec_res, 0});
				std::this_thread::sleep_for(std::chrono::milliseconds(150));
				LOG(INFO) << acc;
			}*/
			//deceleration stage 
			//else if ((abs(cur_east) >= abs(target_east)*0.80 && ret == false) || (abs(cur_east) <= abs(target_east)*0.80 && ret == true)){
			//else if (abs(abs(target_east)- abs(cur_east)) <= east_distance*0.25){
			else if (abs(abs(target_east)- abs(cur_east)) <= decel_distance){
				if(dcc-0.5>=0.5)
					dcc = dcc-0.4;
				else
					dcc = 0.5;

				float vec_n = target_north - cur_north;
				float vec_e = target_east - cur_east;
				float vec_d = target_down - cur_down;
				float vec_res = sqrt(pow(vec_n,2)+pow(vec_e,2)+pow(vec_d,2));
				update_payload(command::DECEL_EAST, dcc);
				offboard.set_velocity_ned((mavsdk::Offboard::VelocityNedYaw){vec_n/vec_res, dcc*vec_e/vec_res, vec_d/vec_res, 0});
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				LOG(INFO) << dcc;
			}
			else {
				while(first){
					j = abs(cur_east) + east_increment;
					first = false;
					LOG(INFO) << "Inside position phase...";
				}
				switch (ret){
					case 0:
						if ((abs(cur_east) >= j - east_next_point) && (abs(cur_east) + east_increment <= abs(target_east*0.75))) {
							j = j + east_increment;
						} 
						else if (abs(cur_east) + east_increment > abs(target_east)){
							j = abs(target_east);
						}
						break;
					case 1:
						if ((abs(cur_east) <= j - east_next_point) && (abs(cur_east) + east_increment >= abs(target_east*0.75))) {
							j = j + east_increment;
						} 
						else if (abs(cur_east) + east_increment < abs(target_east)){
							j = abs(target_east);
						}
						break;					
				}
				LOG(INFO) << j;
				update_payload(command::POSITION, target_north, east_quad*j, target_down, 0, 0, 0);
				offboard.set_position_ned((mavsdk::Offboard::PositionNedYaw){target_north, east_quad*j, target_down, 0});
			}
		}

		if (target && !comm_fail.load()){
			LOG(INFO) << "Target position reached\n";
			return 0;
		}
		else if (comm_fail.load()){
			LOG(INFO) << "Communication failed. Starting emergency hold. \n";
			return -1;
		}
		return 0;
}


int main (){

	//logger config
	//TODO:Enable different logger options or use boost logging
	google::SetLogDestination(google::INFO, "/home/ubuntu/drone/logs/master.log"); // Sets log destination and filename
	FLAGS_alsologtostderr = 1;
	google::InitGoogleLogging("master");

	float target_north, target_east, target_down;
	std::cout << "Enter target north direction (m): ";
	std::cin >> target_north;
	std::cout << "Enter target east direction (m): ";
	std::cin >> target_east;
	std::cout << "Enter target down direction (m): ";
	std::cin >> target_down;

	//TODO: Handle bad data from cin 

	if (target_down > 0 && target_down <= max_alt)
		target_down = target_down * -1;
	else if ((target_down < 0 && target_down < -1*max_alt) || (target_down > 0 && target_down > max_alt)){
		LOG(INFO) << "Maximum allowed altitude is: " << max_alt;
		return -1;
	}


	LOG(INFO) << "Target in NED: " << target_north << ", " << target_east << ", " << target_down << "\n";

	//create a Mavsdk class object mavsdk with Mavsdk::Configuration class object as constructor argurment to mavsdk. Constructor argument has been made mandatory
	mavsdk::Mavsdk::Configuration config{mavsdk::Mavsdk::ComponentType::CompanionComputer};
	config.set_always_send_heartbeats(true);
	mavsdk::Mavsdk mavsdk{config};
	//mavsdk::Mavsdk mavsdk{mavsdk::Mavsdk::Configuration{mavsdk::Mavsdk::ComponentType::CompanionComputer}};
	mavsdk::ConnectionResult connection_result = mavsdk.add_any_connection("serial:///dev/serial0:115200");//ConnectionResult is public enum in mavsdk namespace	
	//mavsdk::ConnectionResult connection_result = mavsdk.add_any_connection("udp://0.0.0.0:14540");
	if(connection_result != mavsdk::ConnectionResult::Success){
	    LOG(INFO) << "Connection failed: " << connection_result << "\n";
	    return -1;
	}

	auto system = mavsdk.first_autopilot(3.0);
	if(!system){
	    LOG(INFO) << "Timed out waiting for system \n";
	    return -1;
	}

	//Instantiate plugins
	auto action = mavsdk::Action{system.value()};
	auto offboard = mavsdk::Offboard{system.value()};
	auto telemetry = mavsdk::Telemetry{system.value()};

	while(!telemetry.health_all_ok()){
	    LOG(INFO) << "Waiting for system to be ready \n";
	    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	LOG(INFO) << "System is ready\n";
	
	update_payload(command::ESTABLISH_CONNECTION);

	LOG(INFO) << "Starting communication thread...\n";
	std::thread comm_thread(comm_module);
	std::this_thread::sleep_for(std::chrono::seconds(1));	


	while (comm_fail.load()){
	    LOG(INFO) << "Waiting for all drones to connect...\n";
	    std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	
	LOG(INFO) << "All drones connected \n";
	
	telemetry.set_rate_position_velocity_ned(120);
	mavsdk::Telemetry::PositionVelocityNedHandle pos_handle = telemetry.subscribe_position_velocity_ned(position_update);
	mavsdk::Telemetry::FlightModeHandle flight_mode_handle = telemetry.subscribe_flight_mode(flight_mode_update);

	update_payload(command::TARGET, target_north, target_east, target_down);
	std::this_thread::sleep_for(std::chrono::seconds(2));

	update_payload(command::ARM);
	//std::this_thread::sleep_for(std::chrono::seconds(10));
	const auto arm_result = action.arm();
	if (arm_result != mavsdk::Action::Result::Success){
	    //emergency_hold(action, comm_thread, con_chk_thread);
	    LOG(INFO) << "Arming failed: " << arm_result << '\n';
	    return 1;
	}
	LOG(INFO) << "Armed\n";

	std::this_thread::sleep_for(std::chrono::seconds(1));


	update_payload(command::INITIAL_SETPOINT);
	LOG(INFO) << "Set Initial setpoint\n";
	const mavsdk::Offboard::VelocityNedYaw stay{};
	offboard.set_velocity_ned(stay);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	if (comm_fail.load())
	    return emergency_hold(action, comm_thread, telemetry, pos_handle);


	update_payload(command::OFFBOARD_START);
	const auto offboard_start_result = offboard.start();
	if (offboard_start_result != mavsdk::Offboard::Result::Success) {
	    LOG(INFO) << "Offboard start failed: " << offboard_start_result << "\n";
	    return 1;
	}
	LOG(INFO) << "Offboard started\n";

	if (comm_fail.load())
		return emergency_hold(action, comm_thread, telemetry, pos_handle);

	update_payload(command::TAKEOFF);
	//pos_update.store(true);
	//std::this_thread::sleep_for(std::chrono::seconds(1));	
	float cur_north = 0; float cur_east = 0; float cur_down = 0;
	float start_north, start_east;
	{
		std::lock_guard<std::mutex> lock_pos(position_mutex);
		start_north = position.field1;
		start_east = position.field2;
	}
	LOG(INFO) << "Go to target altitude\n";

	{
		bool target = false;
		while(!target && !comm_fail.load()){
			{
				std::lock_guard<std::mutex> lock_pos(position_mutex);
				cur_north = position.field1;
				cur_east = position.field2;
				cur_down = position.field3;
			}

			if(target_down - cur_down > -1*alt_err_margin){
				target = true;
			}
			else{
				//LOG(INFO) << cur_down;
				offboard.set_position_ned((mavsdk::Offboard::PositionNedYaw){start_north, start_east, target_down, 0});
			}
		}

		if (target && !comm_fail.load())
			LOG(INFO) << "Target altitude reached \n";
		else if (comm_fail.load()){
			LOG(INFO) << "Communication failed. Starting emergency hold. \n";
			return emergency_hold(action, comm_thread, telemetry, pos_handle);
		}
	}


	int north_result, east_result;
	update_payload(command::TARGET, target_north, 0, target_down);
	std::this_thread::sleep_for(std::chrono::seconds(1));
	north_result = move_north(offboard, target_north, 0, target_down);
	if (north_result == -1)
		return emergency_hold(action, comm_thread, telemetry, pos_handle);

	update_payload(command::TARGET, target_north, target_east, target_down);
	std::this_thread::sleep_for(std::chrono::seconds(1));
	east_result = move_east(offboard, target_north, target_east, target_down);
	if (east_result == -1)
		return emergency_hold(action, comm_thread, telemetry, pos_handle);


	update_payload(command::POSITION, target_north, target_east, target_down);
	offboard.set_position_ned((mavsdk::Offboard::PositionNedYaw){target_north, target_east, target_down, 0});

	/*
	update_payload(command::TARGET, 0, target_east, target_down);
	std::this_thread::sleep_for(std::chrono::seconds(1));
	north_result = move_north(offboard, 0, target_east, target_down);
	if (north_result == -1)
		return emergency_hold(action, comm_thread, telemetry, pos_handle);

	update_payload(command::TARGET, 0, 0, target_down);
	std::this_thread::sleep_for(std::chrono::seconds(1));
	east_result = move_east(offboard, 0, 0, target_down);
	if (east_result == -1)
		return emergency_hold(action, comm_thread, telemetry, pos_handle);

	*/

	if(comm_fail.load())
		return emergency_hold(action, comm_thread, telemetry, pos_handle);

	std::this_thread::sleep_for(std::chrono::seconds(10));

	if(comm_fail.load())
		return emergency_hold(action, comm_thread, telemetry, pos_handle);

	//Descend slowly down using offboard
	LOG(INFO) << "Descending to 2m altitude...\n";
	{
		float descend_north, descend_east, descend_down;
		{
			std::lock_guard<std::mutex> lock_pos(position_mutex);
			descend_north = position.field1;
			descend_east = position.field2;
			descend_down = position.field3;
		}
		bool target = false;
		update_payload(command::POSITION, descend_north, descend_east, -2, 0, 0, 0);
		while(!target && !comm_fail.load()){
			{
				std::lock_guard<std::mutex> lock_pos(position_mutex);
				cur_down = position.field3;
			}

			if(cur_down >= -2.2 && cur_down <= -1.8){	
				target = true;
			}
			else {
				offboard.set_position_ned((mavsdk::Offboard::PositionNedYaw){descend_north, descend_east, -2, 0});
			}
		}	
	}

	if(comm_fail.load())
		return emergency_hold(action, comm_thread, telemetry, pos_handle);


	update_payload(command::OFFBOARD_STOP);
	const auto offboard_stop_result = offboard.stop();
	if (offboard_stop_result != mavsdk::Offboard::Result::Success){
	    LOG(INFO) << "Offboard stop failed: " << offboard_stop_result << "\n";
	    return emergency_hold(action, comm_thread, telemetry, pos_handle);
	}

	if (comm_fail.load()){
		return emergency_hold(action, comm_thread, telemetry, pos_handle);
	}

	//std::this_thread::sleep_for(std::chrono::seconds(10));

	update_payload(command::LAND);
	LOG(INFO) << "Landing...\n";
	const auto land_result = action.land();
	if(land_result != mavsdk::Action::Result::Success){
		LOG(INFO) << "Landing failed..." << land_result << "\n";
		return emergency_hold(action, comm_thread, telemetry, pos_handle);
	}
	
	mission_complete.store(true);

	while(telemetry.landed_state() != mavsdk::Telemetry::LandedState::OnGround){
		; //wait till it is landed
	}


	update_payload(command::DISARM);
	LOG(INFO) << "Disarm...\n";
	const auto disarm_result = action.disarm();
	if(disarm_result != mavsdk::Action::Result::Success){
		LOG(INFO) << "Disarming failed..." << disarm_result << "\n";
	}

	std::this_thread::sleep_for(std::chrono::seconds(5));
	
	
	//close communication thread
	LOG(INFO) << "Did communication fail: " << comm_fail.load();
	main_thread_done.store(true);
	std::this_thread::sleep_for(std::chrono::seconds(1));
	comm_thread.join();
	telemetry.unsubscribe_position_velocity_ned(pos_handle);
	return 0;


}
