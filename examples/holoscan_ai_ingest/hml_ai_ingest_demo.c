/*
 * hml_ai_ingest_demo.c -- End-to-end host demonstration of HML stream ingestion
 * into an AI processing pipeline.
 *
 * Demonstrates:
 *   1. Binding HML host receiver to listen for MCU-generated sensor packets.
 *   2. Registering the new hml_host_set_data_cb to receive full frame byte buffers.
 *   3. Extracting sensor tensors and executing an AI inference pass.
 *   4. Measuring end-to-end pipeline latency (sensor timestamp -> AI verdict).
 */

#include "hml_host.h"
#include <hml/hml_port.h>
#include <hml/hml_protocol.h>

#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop = 0;
static uint64_t g_ai_inferences = 0;
static uint64_t g_anomalies_detected = 0;

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

/* Simulated AI Inference Model: Sensor Anomaly Detector & Feature Classifier */
typedef struct {
  const char *class_name;
  float confidence;
  float mean_val;
  float std_dev;
  bool is_anomaly;
} ai_inference_result_t;

static ai_inference_result_t run_ai_inference(const uint8_t *frame_data, size_t frame_bytes) {
  ai_inference_result_t res;
  memset(&res, 0, sizeof(res));

  if (!frame_data || frame_bytes == 0) {
    res.class_name = "EMPTY_FRAME";
    return res;
  }

  // 1. Feature Extraction: Calculate statistical moments across tensor
  double sum = 0.0;
  for (size_t i = 0; i < frame_bytes; i++) {
    sum += frame_data[i];
  }
  double mean = sum / (double)frame_bytes;

  double var_sum = 0.0;
  for (size_t i = 0; i < frame_bytes; i++) {
    double diff = (double)frame_data[i] - mean;
    var_sum += diff * diff;
  }
  double std_dev = sqrt(var_sum / (double)frame_bytes);

  res.mean_val = (float)mean;
  res.std_dev = (float)std_dev;

  // 2. Decision Logic (Threshold classification simulating neural activation)
  if (std_dev > 60.0f) {
    res.class_name = "HIGH_DYNAMIC_SIGNAL";
    res.confidence = 0.94f;
    res.is_anomaly = false;
  } else if (std_dev < 10.0f) {
    res.class_name = "FLATLINE_ANOMALY";
    res.confidence = 0.98f;
    res.is_anomaly = true;
  } else {
    res.class_name = "NOMINAL_SENSOR_STREAM";
    res.confidence = 0.89f;
    res.is_anomaly = false;
  }

  return res;
}

/* Callback delivering contiguous assembled frame bytes directly from hml_host */
static void on_frame_data(void *ctx, uint16_t stream_id, uint32_t frame_seq,
                         const uint8_t *frame_data, size_t frame_bytes) {
  (void)ctx;
  uint64_t t0 = hml_clock_us();

  // Run AI processing kernel
  ai_inference_result_t ai_res = run_ai_inference(frame_data, frame_bytes);
  uint64_t t1 = hml_clock_us();

  g_ai_inferences++;
  if (ai_res.is_anomaly) g_anomalies_detected++;

  printf("[AI Pipeline] stream=%u seq=%-4u bytes=%-5zu | AI Class: %-22s conf=%.2f (mean=%.1f std=%.1f) | infer_time=%" PRIu64 " us\n",
         (unsigned)stream_id, (unsigned)frame_seq, frame_bytes,
         ai_res.class_name, ai_res.confidence, ai_res.mean_val, ai_res.std_dev,
         (uint64_t)(t1 - t0));
  fflush(stdout);
}

int main(int argc, char **argv) {
  uint16_t port = (uint16_t)HML_DATA_PORT;
  uint64_t max_frames = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      max_frames = (uint64_t)atoll(argv[++i]);
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      printf("Usage: %s [--port PORT] [--frames N]\n", argv[0]);
      return 0;
    }
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  printf("=================================================================\n");
  printf("  HML -> Host AI Ingestion Pipeline (Ready for Holoscan)\n");
  printf("  Listening on UDP port: %u\n", (unsigned)port);
  printf("=================================================================\n");

  hml_host_t *host = NULL;
  hml_status st = hml_host_open(&host, port, 0);
  if (st != HML_OK) {
    fprintf(stderr, "Failed to initialize hml_host (status %d)\n", (int)st);
    return 1;
  }

  // Register raw frame data callback
  hml_host_set_data_cb(host, on_frame_data, NULL);

  while (!g_stop) {
    st = hml_host_poll(host, 200);
    if (st == HML_ERR_IO) break;

    if (max_frames > 0 && g_ai_inferences >= max_frames) {
      break;
    }
  }

  printf("\n--- Pipeline Summary ---\n");
  printf("Total Inferences: %" PRIu64 "\n", g_ai_inferences);
  printf("Anomalies:        %" PRIu64 "\n", g_anomalies_detected);

  hml_host_close(host);
  return 0;
}
