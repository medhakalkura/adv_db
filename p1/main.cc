#include "iceberg_table.h"
#include <time.h>
#include <thread>
#include <immintrin.h>
#include <tmmintrin.h>
#include <openssl/rand.h>
#include <unistd.h>
#include <chrono>
#include <random>
#include <algorithm>
#include <string.h>
#include <assert.h>

using namespace std::chrono;

iceberg_table * table;

double elapsed(high_resolution_clock::time_point t1, high_resolution_clock::time_point t2) {
  return (duration_cast<duration<double>>(t2 - t1)).count();
}

void do_inserts(uint8_t id, uint64_t *keys, uint64_t *values, uint64_t start, uint64_t n) {

  for(uint64_t i = start; i < start + n; ++i)
    if(!iceberg_insert(table, keys[i], values[i], id)) {
      printf("Failed insert\n");
      exit(0);
    }
}

void do_queries(uint64_t *keys, uint64_t start, uint64_t n, bool positive) {

  uint64_t *val;
  for(uint64_t i = start; i < start + n; ++i)
    if (iceberg_get_value(table, keys[i], &val) != positive) {

      if(positive) printf("False negative query\n");
      else printf("False positive query\n");
      exit(0);
    }
}

void do_removals(uint8_t id, uint64_t *keys, uint64_t start, uint64_t n) {

  //uint64_t val;

  for(uint64_t i = start; i < start + n; ++i)
    if(!iceberg_remove(table, keys[i], id)) {
      printf("Failed removal\n");
      exit(0);
    }
}

void safe_rand_bytes(unsigned char *v, size_t n) {
  while (n > 0) {
    size_t round_size = n >= INT_MAX ? INT_MAX - 1 : n;
    RAND_bytes(v, round_size);
    v += round_size;
    n -= round_size;
  }
}

void do_mixed(uint8_t id, std::vector<std::pair<uint64_t, uint64_t>>& v, uint64_t start, uint64_t n) {

  uint64_t *val;
  for(uint64_t i = start; i < start + n; ++i)
    if(iceberg_get_value(table, v[i].first, &val)) {

      iceberg_remove(table, v[i].first, id);
    } else iceberg_insert(table, v[i].first, v[i].second, id);
}

int main (int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    fprintf(stderr, "Specify the log of the number of slots in the table and the number of threads to use.\n");
    exit(1);
  }

  bool is_benchmark = false;
  if (argc == 4) {
    assert(strcmp(argv[3], "-b") == 0);
    is_benchmark = true;
  }

  uint64_t tbits = atoi(argv[1]);
  uint64_t threads = atoi(argv[2]);
  uint64_t N = (1ULL << tbits) * 1.07;

  high_resolution_clock::time_point t1 = high_resolution_clock::now();

  if ((table = iceberg_init(tbits)) == NULL) {
    fprintf(stderr, "Can't allocate iceberg table.\n");
    exit(EXIT_FAILURE);
  }

  high_resolution_clock::time_point t2 = high_resolution_clock::now();
  
  srand(time(NULL));

  //Generating vectors of size N for data contained and not contained in the tablea
  //
  uint64_t splits = 1;

  uint64_t size = N / splits / threads;

  N = N / size * size;

  uint64_t *in_keys = (uint64_t *)malloc(N * sizeof(uint64_t));
  if(!in_keys) {
    printf("Malloc in_keys failed\n");
    exit(0);
  }
  safe_rand_bytes((unsigned char *)in_keys, sizeof(*in_keys) * N);

  uint64_t *in_values = (uint64_t *)malloc(N * sizeof(uint64_t));
  if(!in_values) {
    printf("Malloc in_values failed\n");
    exit(0);
  }
  safe_rand_bytes((unsigned char *)in_values, sizeof(*in_values) * N);

  uint64_t *out_keys = (uint64_t *)malloc(N * sizeof(uint64_t));
  if(!out_keys) {
    printf("Malloc out_keys failed\n");
    exit(0);
  }
  safe_rand_bytes((unsigned char *)out_keys, sizeof(*out_keys) * N);

  uint64_t *out_values = (uint64_t *)malloc(N * sizeof(uint64_t));
  if(!out_values) {
    printf("Malloc out_values failed\n");
    exit(0);
  }
  safe_rand_bytes((unsigned char *)out_values, sizeof(*out_values) * N);

  if (!is_benchmark) {
    printf("INSERTIONS\n");
  }

  t1 = high_resolution_clock::now();

  std::vector<std::thread> thread_list;
  for(uint64_t i = 0; i < splits; ++i) {
    for(uint64_t j = 0; j < threads; j++)
      thread_list.emplace_back(do_inserts, j, in_keys, in_values, (i * threads + j) * size, size);
    for(uint64_t j = 0; j < threads; j++)
      thread_list[j].join();
    thread_list.clear();
  }

  t2 = high_resolution_clock::now();

  double insert_throughput = N / elapsed(t1, t2);
  if (!is_benchmark) {
    printf("Insertions: %f /sec\n", N / elapsed(t1, t2));
    printf("Load factor: %f\n", iceberg_load_factor(table));
    
    printf("Number level 1 inserts: %ld\n", lv1_balls(table));
    printf("Number level 2 inserts: %ld\n", lv2_balls(table));
    printf("Number level 3 inserts: %ld\n", lv3_balls(table));
    printf("Total inserts: %ld\n", tot_balls(table));
    
    printf("\nQUERIES\n");
  }

  std::mt19937 g(__builtin_ia32_rdtsc());
  std::shuffle(&in_keys[0], &in_keys[N], g);

  t1 = high_resolution_clock::now();

  for(uint64_t i = 0; i < threads; ++i)
    thread_list.emplace_back(do_queries, out_keys, i * (N / threads), N / threads, false);
  for(uint64_t i = 0; i < threads; ++i)
    thread_list[i].join();

  t2 = high_resolution_clock::now();
  double negative_throughput = N / elapsed(t1, t2);
  if (!is_benchmark) {
    printf("Negative queries: %f /sec\n", N / elapsed(t1, t2));	
  }
  thread_list.clear();

  t1 = high_resolution_clock::now();

  for(uint64_t i = 0; i < threads; ++i)
    thread_list.emplace_back(do_queries, in_keys, i * (N / threads), N / threads, true);
  for(uint64_t i = 0; i < threads; ++i)
    thread_list[i].join();

  t2 = high_resolution_clock::now();
  double positive_throughput = N / elapsed(t1, t2);
  if (!is_benchmark) {
    printf("Positive queries: %f /sec\n", N / elapsed(t1, t2));
  }
  thread_list.clear();

  if (!is_benchmark) {
    printf("\nREMOVALS\n");
  }

  uint64_t num_removed = N / 2 / threads * threads;
  uint64_t *removed = in_keys;
  uint64_t *non_removed = in_keys + num_removed;

  shuffle(&removed[0], &removed[num_removed], g);
  shuffle(&non_removed[0], &non_removed[N - num_removed], g);

  t1 = high_resolution_clock::now();

  for(uint64_t i = 0; i < threads; ++i)
    thread_list.emplace_back(do_removals, i, removed, i * (N / 2 / threads), N / 2 / threads);
  for(uint64_t i = 0; i < threads; ++i)
    thread_list[i].join();

  t2 = high_resolution_clock::now();
  double removal_throughput = num_removed / elapsed(t1, t2);
  if (!is_benchmark) {
    printf("Removals: %f /sec\n", num_removed / elapsed(t1, t2));
    printf("Load factor: %f\n", iceberg_load_factor(table));
  }
  thread_list.clear();

  shuffle(&removed[0], &removed[num_removed], g);

  t1 = high_resolution_clock::now();

  if (is_benchmark) {
    printf("%f %f %f %f\n", insert_throughput, negative_throughput, positive_throughput, removal_throughput);
    return 0;
  }

  for(uint64_t i = 0; i < threads; ++i)
    thread_list.emplace_back(do_queries, removed, i * (N / 2 / threads), N / 2 / threads, false);
  for(uint64_t i = 0; i < threads; ++i)
    thread_list[i].join();

  t2 = high_resolution_clock::now();
  printf("Negative queries after removals: %f /sec\n", num_removed / elapsed(t1, t2));
  thread_list.clear();

  t1 = high_resolution_clock::now();

  for(uint64_t i = 0; i < threads; ++i)
    thread_list.emplace_back(do_queries, non_removed, i * (N / 2 / threads), N / 2 / threads, true);
  for(uint64_t i = 0; i < threads; ++i)
    thread_list[i].join();

  t2 = high_resolution_clock::now();
  printf("Positive queries after removals: %f /sec\n", num_removed / elapsed(t1, t2));
  thread_list.clear();
}
