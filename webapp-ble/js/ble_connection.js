// BLE transport for the Emotiscope remote - replaces websockets_connection.js.
//
// Speaks the same pipe-delimited command protocol over the Nordic UART Service
// (see src/ble_transport.h in the firmware). Commands are newline-framed in
// both directions so they survive BLE packet fragmentation.
//
// Requires Web Bluetooth: Chrome/Edge on Android, Windows, macOS, Linux.
// (No iOS Safari - use the Bluefy browser there.)

const MAX_PING_PONG_REPLY_TIME_MS = 4000;
const BLE_NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const BLE_NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // we write commands here
const BLE_NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // we get notifications here
const BLE_WRITE_CHUNK = 20;          // safe for the 23-byte minimum MTU; firmware reassembles on '\n'
const BLE_RECONNECT_ATTEMPTS = 5;

let ble_device = null;
let ble_write_char = null;
let ble_connected = false;
let ble_intentional_disconnect = false;

let last_ping_time;
let pong_pending = false;
let pongs_halted = false;
let standby_mode = false;
let pong_watchdog_started = false;

let rx_text_buffer = "";
let tx_queue = Promise.resolve();
const text_encoder = new TextEncoder();
const text_decoder = new TextDecoder();

let touch_vals = [0, 0, 0];
let touch_low  = [0, 0, 0];
let touch_high = [0, 0, 0];

got_touch_vals = true;

let auto_response_table = {
	"welcome"      :"get|config",
	"config_ready" :"get|modes",
	"modes_ready"  :"get|sliders",
	"sliders_ready":"get|toggles",
	"toggles_ready":"get|menu_toggles",
	"reload_config":"get|config",
};

// ----------------------------------------------------------------------------
// Connection management
// ----------------------------------------------------------------------------

function set_connect_overlay_visible(visible, message){
	let overlay = document.getElementById("ble_connect_overlay");
	if(overlay){
		overlay.style.display = visible ? "flex" : "none";
	}
	let status = document.getElementById("ble_connect_status");
	if(status && message != undefined){
		status.innerHTML = message;
	}
}

// Must be called from a user gesture (the CONNECT button) - Web Bluetooth
// refuses to show the device chooser otherwise
async function connect_ble(){
	if(!navigator.bluetooth){
		set_connect_overlay_visible(true,
			"This browser has no Web Bluetooth.<br>Use Chrome or Edge (Android / desktop).");
		return;
	}

	try{
		set_connect_overlay_visible(true, "Choose your Emotiscope...");
		ble_device = await navigator.bluetooth.requestDevice({
			filters: [{ services: [BLE_NUS_SERVICE] }]
		});
		ble_device.addEventListener("gattserverdisconnected", on_ble_disconnected);
	}
	catch(e){
		console.log("BLE CHOOSER CANCELLED/FAILED: " + e);
		set_connect_overlay_visible(true, "Connection failed or cancelled.");
		return;
	}

	// On Windows (recent Bluetooth drivers) the first GATT connection to a
	// freshly chosen device almost always fails - retry once automatically.
	// Re-using the already-granted device needs no extra user gesture.
	const INITIAL_CONNECT_TRIES = 2;
	for(let attempt = 1; attempt <= INITIAL_CONNECT_TRIES; attempt++){
		try{
			if(attempt > 1){
				set_connect_overlay_visible(true, `Retrying (${attempt}/${INITIAL_CONNECT_TRIES})...`);
			}
			await open_ble_connection();
			return;
		}
		catch(e){
			console.log(`BLE CONNECT ATTEMPT ${attempt} FAILED: ${e}`);
			try{ ble_device.gatt.disconnect(); }catch(_){ }
			await new Promise(resolve => setTimeout(resolve, 500));
		}
	}
	set_connect_overlay_visible(true, "Connection failed - press CONNECT to try again.");
}

async function open_ble_connection(){
	set_connect_overlay_visible(true, "Connecting...");

	const server = await ble_device.gatt.connect();
	const service = await server.getPrimaryService(BLE_NUS_SERVICE);
	ble_write_char = await service.getCharacteristic(BLE_NUS_RX);
	const notify_char = await service.getCharacteristic(BLE_NUS_TX);

	notify_char.addEventListener("characteristicvaluechanged", on_ble_notification);
	await notify_char.startNotifications(); // firmware replies "welcome" once this lands

	ble_connected = true;
	rx_text_buffer = "";
	tx_queue = Promise.resolve();

	console.log("[ble] connected");
	set_connect_overlay_visible(false);

	let nickname = document.getElementById("device_nickname");
	if(nickname){ nickname.innerHTML = "BLUETOOTH"; }

	transmit("get|version");
}

function on_ble_disconnected(){
	console.log("[ble] disconnected");
	const was_fully_connected = ble_connected;
	ble_connected = false;
	pong_pending = false;

	// Only auto-reconnect when an *established* connection drops. A failed
	// initial attempt also fires this event, and its retry logic lives in
	// connect_ble() - running both at once would race on the GATT server.
	if(was_fully_connected == true && ble_intentional_disconnect == false){
		set_ui_locked_state(true);
		attempt_ble_reconnect();
	}
}

// Reconnecting to an already-granted device needs no user gesture,
// so try silently a few times before falling back to the button
async function attempt_ble_reconnect(){
	for(let attempt = 1; attempt <= BLE_RECONNECT_ATTEMPTS; attempt++){
		set_connect_overlay_visible(true, `Reconnecting (${attempt}/${BLE_RECONNECT_ATTEMPTS})...`);
		try{
			await open_ble_connection();
			set_ui_locked_state(false);
			return;
		}
		catch(e){
			console.log(`[ble] reconnect attempt ${attempt} failed: ${e}`);
			await new Promise(resolve => setTimeout(resolve, 1000 * attempt));
		}
	}
	set_connect_overlay_visible(true, "Connection lost.");
}

// ----------------------------------------------------------------------------
// Receive path: notification chunks -> newline-framed messages
// ----------------------------------------------------------------------------

function on_ble_notification(event){
	rx_text_buffer += text_decoder.decode(event.target.value);

	let newline_index;
	while((newline_index = rx_text_buffer.indexOf("\n")) >= 0){
		const message = rx_text_buffer.slice(0, newline_index).replace(/\r$/, "");
		rx_text_buffer = rx_text_buffer.slice(newline_index + 1);
		if(message.length > 0){
			parse_message(message);
		}
	}
}

// ----------------------------------------------------------------------------
// Transmit path: serialized, chunked GATT writes
// ----------------------------------------------------------------------------

function transmit(message){
	if(ble_connected == false || ble_write_char == null){
		console.log(`TX SKIPPED (not connected): ${message}`);
		return;
	}
	console.log(`TX: ${message}`);

	const payload = text_encoder.encode(message + "\n");

	// GATT allows one operation at a time - chain writes on a promise queue
	tx_queue = tx_queue.then(async () => {
		for(let offset = 0; offset < payload.length; offset += BLE_WRITE_CHUNK){
			const chunk = payload.slice(offset, offset + BLE_WRITE_CHUNK);
			if(ble_write_char.writeValueWithoutResponse){
				await ble_write_char.writeValueWithoutResponse(chunk);
			}
			else{
				await ble_write_char.writeValue(chunk);
			}
		}
	}).catch(e => {
		console.log("BLE WRITE FAILED: " + e);
	});
}

// ----------------------------------------------------------------------------
// Liveness (ping/pong) - unchanged protocol, BLE supervision does the rest
// ----------------------------------------------------------------------------

function ping_server(){
	transmit("ping");
	last_ping_time = performance.now();
	pong_pending = true;
}

function check_pong_timeout(){
	if(pongs_halted == false && ble_connected == true){
		if(pong_pending == true){
			if(performance.now() - last_ping_time >= MAX_PING_PONG_REPLY_TIME_MS){
				console.log("NO PONG WITHIN TIMEOUT!");
				pong_pending = false;
				try{ ble_device.gatt.disconnect(); } // triggers the reconnect path
				catch(e){ on_ble_disconnected(); }
			}
		}
	}
}

// ----------------------------------------------------------------------------
// Everything below is unchanged from websockets_connection.js
// ----------------------------------------------------------------------------

function set_ui_locked_state(locked_state){
	let dimmer = document.getElementById("dimmer");
	if(locked_state == true){
		dimmer.style.opacity = 1.0;
		dimmer.style.pointerEvents = "all";
	}
	else{
		dimmer.style.opacity = 0.0;
		dimmer.style.pointerEvents = "none";
	}
}

function attempt_auto_response(message){
	let success = false;
	try{
		let reply = auto_response_table[message];
		if(reply != undefined){
			console.log(`Auto-reply for message ${message} is: ${reply}`);
			transmit( reply );
			success = true;
		}
	}
	catch(e){
		console.log(e);
	}

	return success;
}

function start_noise_calibration(){
	set_ui_locked_state(true);
	transmit('noise_cal');
}

function start_debug_recording(){
	set_ui_locked_state(true);
	transmit('start_debug_recording');
}

function sync_data_from_device(){
	transmit("get|config"); // Triggers chain of data sync commands
}

function parse_message(message){
	if( attempt_auto_response(message) == false){
		// parse reply contents
		let command_data = message.split("|");
		let command_type = command_data[0];

		if(command_type == "clear_config"){
			// Clear client-side config JSON
			configuration = {};

			set_ui_locked_state(true);
		}
		else if(command_type == "new_config"){
			// Append new config key to client-side config JSON
			let config_key_name  = command_data[1];
			let config_data_type = command_data[2];
			let config_value_raw = command_data[3];
			let config_value;

			if(config_data_type == "string"){
				config_value = config_value_raw;
			}
			else if(config_data_type == "float"){
				config_value = parseFloat(config_value_raw);
			}
			else if(config_data_type == "int"){
				config_value = parseInt(config_value_raw);
			}
			else{
				console.log(`UNRECOGNIZED CONFIG DATA TYPE: ${config_data_type}`);
			}

			configuration[config_key_name] = config_value;
		}
		else if(command_type == "clear_modes"){
			modes = [];
		}
		else if(command_type == "new_mode"){
			let mode_index = parseInt(command_data[1]);
			let mode_type  = parseInt(command_data[2]);
			let mode_name  = command_data[3];

			modes.push({
				"mode_index":mode_index,
				"mode_type":mode_type,
				"mode_name":mode_name
			});
		}
		else if(command_type == "clear_sliders"){
			// Force close UI if it's open
			transmit(`touch_end`);
			transmit(`slider_touch_end`);

			sliders = [];
		}
		else if(command_type == "new_slider"){
			let slider_name = command_data[1];
			let slider_min  = parseFloat(command_data[2]);
			let slider_max  = parseFloat(command_data[3]);
			let slider_step = parseFloat(command_data[4]);

			sliders.push({
				"name":slider_name,
				"min":slider_min,
				"max":slider_max,
				"step":slider_step
			});
		}
		else if(command_type == "clear_toggles"){
			toggles = [];
		}
		else if(command_type == "new_toggle"){
			let toggle_name = command_data[1];

			toggles.push({
				"name":toggle_name
			});
		}
		else if(command_type == "clear_menu_toggles"){
			menu_toggles = [];
		}
		else if(command_type == "new_menu_toggle"){
			let toggle_name = command_data[1];

			menu_toggles.push({
				"name":toggle_name
			});
		}
		else if(command_type == "menu_toggles_ready"){
			//console.log("DATA SYNC COMPLETE!");
			ping_server();
			if(pong_watchdog_started == false){
				pong_watchdog_started = true;
				setInterval(check_pong_timeout, 100);
			}
			render_controls();
			set_ui_locked_state(false);
		}
		else if(command_type == "noise_cal_ready"){
			hide_page('page_calibration');
			set_ui_locked_state(false);
		}
		else if(command_type == "debug_recording_ready"){
			hide_page('page_calibration');
			set_ui_locked_state(false);
		}
		else if(command_type == "fps_cpu"){
			let FPS = command_data[1];
			document.getElementById("CPU_FPS").innerHTML = `CPU FPS: ${FPS}`;
		}
		else if(command_type == "fps_gpu"){
			let FPS = command_data[1];
			document.getElementById("GPU_FPS").innerHTML = `GPU FPS: ${FPS}`;
		}
		else if(command_type == "heap"){
			let heap = command_data[1];
			document.getElementById("HEAP").innerHTML = `HEAP: ${heap}`;
		}
		else if(command_type == "pong"){
			pong_pending = false;
			setTimeout(function(){
				ping_server();
			}, MAX_PING_PONG_REPLY_TIME_MS / 2);
		}
		else if(command_type == "touch_vals"){
			touch_vals[0] = parseInt(command_data[1]);
			touch_vals[1] = parseInt(command_data[2]);
			touch_vals[2] = parseInt(command_data[3]);

			got_touch_vals = true;
		}
		else if(command_type == "version"){
			let version = command_data[1];
			document.getElementById("version_number").innerHTML = "Version: "+version;
		}
		else{
			console.log(`Unrecognized command type: ${command_type}`);
		}
	}
}

function set_mode(mode_name){
	transmit(`set|mode|${mode_name}`);
}

function increment_mode(){
	transmit(`increment_mode`);
}

function send_slider_change(slider_name){
	let new_value = document.getElementById(slider_name).value;
	transmit(`set|${slider_name}|${new_value}`);
}

function send_toggle_change(toggle_name){
	let new_state = +(document.getElementById(toggle_name).checked);
	transmit(`set|${toggle_name}|${new_state}`);
}

function send_menu_toggle_change(toggle_name){
	let new_state = +(document.getElementById(toggle_name).checked);
	transmit(`set|${toggle_name}|${new_state}`);
}

// Function to handle touch events on the device icon
function setup_top_touch_listener(div_id) {
    const device_icon = document.getElementById(div_id);
    let touch_timer = null;
    let touch_active = false;

    device_icon.addEventListener('touchstart', function(e) {
        if (touch_active) return; // Ignore if another touch is already active
        touch_active = true;
        touch_timer = setTimeout(function() {
            transmit('button_hold');
			trigger_vibration(100);

			if(standby_mode == false){
				standby_mode = true;
				document.getElementById("header_logo").style.color = "#5495d761";
			}
			else if(standby_mode == true){
				standby_mode = false;
				document.getElementById("header_logo").style.color = "var(--primary)";
			}

            touch_timer = null; // Clear the timer once the function is called
        }, 500); // Set timeout for 500ms
    });

    device_icon.addEventListener('touchend', function(e) {
        if (touch_timer) {
            clearTimeout(touch_timer); // Clear the timer if the touch ends before 500ms
            transmit('button_tap');

			if(standby_mode == true){
				standby_mode = false;
				document.getElementById("header_logo").style.color = "var(--primary)";
			}
        }
        touch_active = false; // Allow new touches
    });
}

(function() {
    var first_load = true;

    // Register the touch event listeners on page load
    document.addEventListener('APP_LOADED', function() {
        if(first_load == true){
            first_load = false;
            console.log("APP_LOADED ble_connection.js");
			setup_top_touch_listener("header_logo");
			setup_top_touch_listener("device_icon");

			// Web Bluetooth needs a user gesture - the overlay's CONNECT
			// button calls connect_ble(), nothing to do here
        }
    });
})();
