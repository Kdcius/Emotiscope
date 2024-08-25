/*
-----------------------------------------------------
  _                                            _
 | |                                          | |
 | |_    ___   _ __ ___    _ __     ___       | |__
 | __|  / _ \ | '_ ` _ \  | '_ \   / _ \      | '_ \ 
 | |_  |  __/ | | | | | | | |_) | | (_) |  _  | | | |
  \__|  \___| |_| |_| |_| | .__/   \___/  (_) |_| |_|
                          | |
                          |_|

Functions related to the computation of possible tempi in music
*/

bool silence_detected = true;
float silence_level = 1.0;

float tempo_confidence = 0.0;

float MAX_TEMPO_RANGE = 1.0;

float tempi_bpm_values_hz[NUM_TEMPI];

float novelty_scale_factor = 1.0;
float novelty_curve[NOVELTY_HISTORY_LENGTH];
float novelty_curve_normalized[NOVELTY_HISTORY_LENGTH];

tempo tempi[NUM_TEMPI];
float tempi_smooth[NUM_TEMPI];
float tempi_power_sum = 0.0;

float last_vu_input = 0.0;

uint16_t find_closest_tempo_bin(float target_bpm) {
	float target_bpm_hz = target_bpm / 60.0;

	float smallest_difference = 10000000.0;
	uint16_t smallest_difference_index = 0;

	for (uint16_t i = 0; i < NUM_TEMPI; i++) {
		float difference = fabs(target_bpm_hz - tempi_bpm_values_hz[i]);
		if (difference < smallest_difference) {
			smallest_difference = difference;
			smallest_difference_index = i;
		}
	}

	return smallest_difference_index;
}

void init_tempo_goertzel_constants() {
	for (uint16_t i = 0; i < NUM_TEMPI; i++) {
		float progress = num_tempi_float_lookup[i];
		float tempi_range = TEMPO_HIGH - TEMPO_LOW;
		float tempo = tempi_range * progress + TEMPO_LOW;

		tempi_bpm_values_hz[i] = tempo / 60.0;
		//Serial.print("TEMPO HZ:");
		//Serial.println(tempi_bpm_values_hz[i]);
	}

	for (uint16_t i = 0; i < NUM_TEMPI; i++) {
		tempi[i].target_tempo_hz = tempi_bpm_values_hz[i];

		float neighbor_left;
		float neighbor_right;

		if (i == 0) {
			neighbor_left = tempi_bpm_values_hz[i];
			neighbor_right = tempi_bpm_values_hz[i + 1];
		}
		else if (i == NUM_TEMPI - 1) {
			neighbor_left = tempi_bpm_values_hz[i - 1];
			neighbor_right = tempi_bpm_values_hz[i];
		}
		else {
			neighbor_left = tempi_bpm_values_hz[i - 1];
			neighbor_right = tempi_bpm_values_hz[i + 1];
		}

		float neighbor_left_distance_hz = fabs(neighbor_left - tempi[i].target_tempo_hz);
		float neighbor_right_distance_hz = fabs(neighbor_right - tempi[i].target_tempo_hz);
		float max_distance_hz = 0;

		if (neighbor_left_distance_hz > max_distance_hz) {
			max_distance_hz = neighbor_left_distance_hz;
		}
		if (neighbor_right_distance_hz > max_distance_hz) {
			max_distance_hz = neighbor_right_distance_hz;
		}

		tempi[i].block_size = NOVELTY_LOG_HZ / (max_distance_hz*0.5);

		if (tempi[i].block_size > NOVELTY_HISTORY_LENGTH) {
			tempi[i].block_size = NOVELTY_HISTORY_LENGTH;
		}

		//Serial.print("TEMPI ");
		//Serial.print(i);
		//Serial.print(" BLOCK SIZE: ");
		//Serial.println(tempi[i].block_size);

		float k = (int)(0.5 + ((tempi[i].block_size * tempi[i].target_tempo_hz) / NOVELTY_LOG_HZ));
		float w = (2.0 * PI * k) / tempi[i].block_size;
		tempi[i].cosine = cos(w);
		tempi[i].sine = sin(w);
		tempi[i].coeff = 2.0 * tempi[i].cosine;

		tempi[i].window_step = 4096.0 / tempi[i].block_size;

		// tempi[i].target_tempo_hz *= 0.5;

		// float radians_per_second = (PI * (tempi[i].target_tempo_hz));
		tempi[i].phase_radians_per_reference_frame = ((2.0 * PI * tempi[i].target_tempo_hz) / (float)(REFERENCE_FPS));

		tempi[i].phase_inverted = false;
	}
	
}

float unwrap_phase(float phase) {
	while (phase - phase > M_PI) {
		phase -= 2 * M_PI;
	}
	while (phase - phase < -M_PI) {
		phase += 2 * M_PI;
	}

	return phase;
}



void calculate_novelty_scale_factor() {
	start_profile(__COUNTER__, __func__);
	
	// Get novelty curve max value for auto-scaling later
	float max_val = 0.0;
	for(uint16_t i = 0; i < NOVELTY_HISTORY_LENGTH; i+=4){
		max_val = fmaxf(max_val, novelty_curve[ i + 0 ]);
		max_val = fmaxf(max_val, novelty_curve[ i + 1 ]);
		max_val = fmaxf(max_val, novelty_curve[ i + 2 ]);
		max_val = fmaxf(max_val, novelty_curve[ i + 3 ]);
	}
	max_val = fmaxf(0.0000000001, max_val);
	float scale_factor_raw = 1.0 / (max_val * 0.5);
	novelty_scale_factor = novelty_scale_factor * 0.7 + scale_factor_raw * 0.3;

	end_profile();
}

void calculate_magnitude_of_tempo(int16_t tempo_bin) {
	start_profile(__COUNTER__, __func__);

	float normalized_magnitude;

	uint16_t block_size = tempi[tempo_bin].block_size;

	float q1 = 0;
	float q2 = 0;

	float window_pos = 0.0;

	for (uint16_t i = 0; i < block_size; i++) {
		uint16_t index = ((NOVELTY_HISTORY_LENGTH - 1) - block_size) + i;

		float sample = novelty_curve[index];
		sample *= novelty_scale_factor;
		clip_float(sample);

		float fade = index / (float)NOVELTY_HISTORY_LENGTH;
		//sample *= fade;

		// convert to AC
		//sample -= 0.5;
		//sample *= 2.0;
		//sample = clip_float(sample);

		float q0 = tempi[tempo_bin].coeff * q1 - q2 + (sample) * window_lookup[(uint32_t)window_pos];
		q2 = q1;
		q1 = q0;

		window_pos += (tempi[tempo_bin].window_step);
	}

	float real = (q1 - q2 * tempi[tempo_bin].cosine);
	float imag = (q2 * tempi[tempo_bin].sine);

	// Calculate phase
	tempi[tempo_bin].phase = atan2(imag, real) + (PI * BEAT_SHIFT_PERCENT);
	
	if (tempi[tempo_bin].phase > PI) {
		tempi[tempo_bin].phase -= (2 * PI);
		tempi[tempo_bin].phase_inverted = !tempi[tempo_bin].phase_inverted;
	}
	else if (tempi[tempo_bin].phase < -PI) {
		tempi[tempo_bin].phase += (2 * PI);
		tempi[tempo_bin].phase_inverted = !tempi[tempo_bin].phase_inverted;
	}

	float magnitude_squared = (q1 * q1) + (q2 * q2) - q1 * q2 * tempi[tempo_bin].coeff;
	float magnitude = sqrt(magnitude_squared);
	normalized_magnitude = magnitude / (block_size / 2.0);

	float progress = 1.0 - (tempo_bin / (float)(NUM_TEMPI));
	progress *= progress;

	float scale = (0.7 * progress) + 0.3;

	//normalized_magnitude *= scale;

	tempi[tempo_bin].magnitude_full_scale = normalized_magnitude;

	end_profile();
}

void update_tempo() {
	start_profile(__COUNTER__, __func__);
	static uint32_t iter = 0;
	iter++;

	if (iter % 10 == 0) {
		calculate_novelty_scale_factor();
	}

	static uint16_t calc_bin = 0;
	uint16_t max_bin = (NUM_TEMPI - 1) * MAX_TEMPO_RANGE;

	calculate_magnitude_of_tempo((calc_bin + 0) % max_bin);
	calculate_magnitude_of_tempo((calc_bin + 1) % max_bin);

	calc_bin+=2;

	if (calc_bin >= max_bin) {
		calc_bin = 0;
	}

	float max_val = 0.0;
	for (uint16_t i = 0; i < NUM_TEMPI; i++) {
		max_val = fmaxf(max_val, tempi[i].magnitude_full_scale);
	}
	float autoranger_scale = 1.0 / max_val;

	for (uint16_t i = 0; i < NUM_TEMPI; i++) {
		float scaled_magnitude = (tempi[i].magnitude_full_scale * autoranger_scale);
		if (scaled_magnitude < 0.0) {
			scaled_magnitude = 0.0;
		}
		if (scaled_magnitude > 1.0) {
			scaled_magnitude = 1.0;
		}

		scaled_magnitude = scaled_magnitude * scaled_magnitude;

		tempi[i].magnitude = scaled_magnitude * scaled_magnitude;
	}

	end_profile();
}

void log_novelty(float input) {
	start_profile(__COUNTER__, __func__);
	shift_array_left(novelty_curve, NOVELTY_HISTORY_LENGTH, 1);
	dsps_mulc_f32_ae32(novelty_curve, novelty_curve, NOVELTY_HISTORY_LENGTH, 0.999, 1, 1);
	novelty_curve[NOVELTY_HISTORY_LENGTH - 1] = input;
	end_profile();
}

void reduce_tempo_history(float reduction_amount) {
	start_profile(__COUNTER__, __func__);
	float reduction_amount_inv = 1.0 - reduction_amount;

	dsps_mulc_f32_ae32(novelty_curve, novelty_curve, NOVELTY_HISTORY_LENGTH, reduction_amount_inv, 1, 1);
	for (uint16_t i = 0; i < NOVELTY_HISTORY_LENGTH; i+=4) {
		novelty_curve[i+0] = fmaxf(novelty_curve[i+0], 0.00000001f);	// never go full zero
		novelty_curve[i+1] = fmaxf(novelty_curve[i+1], 0.00000001f);
		novelty_curve[i+2] = fmaxf(novelty_curve[i+2], 0.00000001f);
		novelty_curve[i+3] = fmaxf(novelty_curve[i+3], 0.00000001f);
	}

	end_profile();
}

void check_silence(float current_novelty) {
	start_profile(__COUNTER__, __func__);
	float min_val = 1.0;
	float max_val = 0.0;
	for (uint16_t i = 0; i < 128; i++) {
		float recent_novelty = novelty_curve[(NOVELTY_HISTORY_LENGTH - 1 - 128) + i] * novelty_scale_factor;
		recent_novelty = fminf(0.5f, recent_novelty) * 2.0;

		float scaled_value = sqrt(recent_novelty);
		max_val = fmaxf(max_val, scaled_value);
		min_val = fminf(min_val, scaled_value);
	}
	float novelty_contrast = fabs(max_val - min_val);
	float silence_level_raw = 1.0 - novelty_contrast;

	silence_level = fmaxf(0.0f, silence_level_raw - 0.5f) * 2.0;

	//float keep_level = 1.0 - silence_level;
	//reduce_tempo_history(silence_level*0.0001);

	if (silence_level_raw > 0.5) {
		silence_detected = true;
	}
	else {
		silence_level = 0.0;
		silence_detected = false;
	}

	// rendered_debug_value = silence_level;
	end_profile();
}

void update_novelty() {
	start_profile(__COUNTER__, __func__);
	static int64_t next_novelty_update = 0.0;

	const float update_interval_hz = NOVELTY_LOG_HZ;
	const int64_t update_interval_us = 1000000 / update_interval_hz;

	if (t_now_us >= next_novelty_update) {
		next_novelty_update += update_interval_us;

		static float fft_last[FFT_SIZE>>1];

		float current_novelty = 0.0;
		for (uint16_t i = 0; i < (FFT_SIZE>>1); i+=1) {
			current_novelty += fmaxf(0.0f, fft_smooth[0][i] - fft_last[i]);
		}
		current_novelty *= current_novelty;

		dsps_memcpy_aes3(fft_last, fft_smooth[0], (FFT_SIZE>>1) * sizeof(float));
		//dsps_memset(fft_max, 0, sizeof(float) * (FFT_SIZE>>1));

		//current_novelty /= 30;

		check_silence(current_novelty);

		//current_novelty = log1p(current_novelty);

		log_novelty(current_novelty);

		vu_max = 0.000001;
	}

	end_profile();
}

void sync_beat_phase(uint16_t tempo_bin, float delta) {
	start_profile(__COUNTER__, __func__);
	float push = (tempi[tempo_bin].phase_radians_per_reference_frame * delta);

	tempi[tempo_bin].phase += push;

	if (tempi[tempo_bin].phase > PI) {
		tempi[tempo_bin].phase -= (2 * PI);
		
		tempi[tempo_bin].phase_inverted = !tempi[tempo_bin].phase_inverted;
	}
	else if (tempi[tempo_bin].phase < -PI) {
		tempi[tempo_bin].phase += (2 * PI);

		tempi[tempo_bin].phase_inverted = !tempi[tempo_bin].phase_inverted;
	}

	end_profile();
}

void update_tempi_phase(float delta) {
	start_profile(__COUNTER__, __func__);
	static bool interlacing_field = 0;
	interlacing_field = !interlacing_field;

	tempi_power_sum = 0.00000001;

	// Iterate over all tempi to smooth them and calculate the power sum
	for (uint16_t tempo_bin = 0; tempo_bin < NUM_TEMPI; tempo_bin++) {
		//if( (tempo_bin % 2) == interlacing_field){
			// Load the magnitude
			float tempi_magnitude = tempi[tempo_bin].magnitude;

			if(tempi_magnitude > 0.005){
				// Smooth it
				tempi_smooth[tempo_bin] = tempi_smooth[tempo_bin] * 0.975 + (tempi_magnitude) * 0.025;
				tempi_power_sum += tempi_smooth[tempo_bin];

				sync_beat_phase(tempo_bin, delta);
			}
			else{
				tempi_smooth[tempo_bin] *= 0.995;
			}
		//}
	}

	// Measure contribution factor of each tempi, calculate confidence level
	float max_contribution = 0.0000001;
	for (uint16_t tempo_bin = 0; tempo_bin < NUM_TEMPI; tempo_bin++) {
		max_contribution = fmaxf(
			tempi_smooth[tempo_bin],
			max_contribution
		);
	}
	tempo_confidence = max_contribution / tempi_power_sum;

	end_profile();
}