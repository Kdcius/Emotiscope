void run_web() {
	profile_function([&]() {
#if TRANSPORT_WIFI
		handle_wifi();
		dns_server.processNextRequest();

		if (web_server_ready == true && wifi_config_mode == false) {
			process_command_queue();
	#if ENABLE_CLOUD_DISCOVERY
			discovery_check_in();
	#endif

			// Write pending changes to LittleFS
			sync_configuration_to_file_system();
		}
#else
		// No WiFi: commands arrive over BLE, but the queue and config
		// persistence still need servicing every loop
		process_command_queue();
		sync_configuration_to_file_system();
#endif

		run_ble_transport(); // no-op when TRANSPORT_BLE is 0
	}, __func__ );
}
