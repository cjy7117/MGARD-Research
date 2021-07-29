/*
 * Copyright 2021, Oak Ridge National Laboratory.
 * MGARD-GPU: MultiGrid Adaptive Reduction of Data Accelerated by GPUs
 * Author: Jieyang Chen (chenj3@ornl.gov)
 * Date: April 2, 2021
 */

#include <chrono>
#include <fstream>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgard_api.h"

#define ANSI_RED "\x1b[31m"
#define ANSI_GREEN "\x1b[32m"
#define ANSI_RESET "\x1b[0m"

using namespace std::chrono;

enum device { CPU, GPU };
enum data_type { SINGLE, DOUBLE };

void print_usage_message(char *argv[], FILE *fp) {
  fprintf(fp,
          "Usage: %s [input file] [num. of dimensions] [1st dim.] [2nd dim.] "
          "[3rd. dim] ... [tolerance] [s]\n",
          argv[0]);
  exit(0);
}

template <typename T>
void min_max(size_t n, T * in_buff) {
  T min = std::numeric_limits<T>::infinity();
  T max = 0;
  for (size_t i = 0; i < n; i++) {
    if (min > in_buff[i]) min = in_buff[i];
    if (max < in_buff[i]) max = in_buff[i];
  }
  printf("Min: %f, Max: %f\n", min, max);
}

void print_config(char * input_file, enum data_type dtype, std::vector<mgard_cuda::SIZE> shape, enum device dev, double tol, double s, enum mgard_cuda::error_bound_type mode) {
  printf("Input data: %s\n", input_file);
  if (dtype == SINGLE) printf("Data type: single precision\n");
  if (dtype == DOUBLE) printf("Data type: double precision\n");
  printf("Shape: %lu ( ", shape.size());
  for (mgard_cuda::DIM d = 0; d < shape.size(); d++) {
    printf("%u ", shape[d]);
  }
  printf(")\n");
  if (mode == mgard_cuda::REL) printf("Error: Relative\n");
  if (mode == mgard_cuda::ABS) printf("Error: Absolute\n");
  printf("Error bound: %.2e ", tol);
  printf("S: %.2f\n", s);
  if (dev == CPU) printf("Use: CPU\n");
  if (dev == GPU) printf("Use: GPU\n");
}

template <typename T>
void readfile(char * input_file, size_t num_bytes, bool check_size, T * in_buff) {
  if (strcmp(input_file, "random") == 0) {
    srand (7117);
    for (int i = 0; i < num_bytes/sizeof(T); i++) {
      in_buff[i] = rand() % 100 + 1;
    }
  } else {
    fprintf(stdout, "Loading file: %s\n", input_file);
    FILE *pFile;
    pFile = fopen(input_file, "rb");
    if (pFile == NULL) {
      fputs("File error", stderr);
      exit(1);
    }
    fseek(pFile, 0, SEEK_END);
    size_t lSize = ftell(pFile);

    rewind(pFile);

    if (check_size && lSize != num_bytes) {
      fprintf(stderr,
              "%s contains %lu bytes when %lu were expected. Exiting.\n",
              input_file, lSize, num_bytes);
      exit(1);
    }

    size_t result = fread(in_buff, 1, num_bytes, pFile);
    if (result != num_bytes) {
      fputs("Reading error", stderr);
      exit(3);
    }
    fclose(pFile);
  }
  min_max(num_bytes/sizeof(T), in_buff);
}


template <mgard_cuda::DIM D, typename T>
void compression(std::vector<mgard_cuda::SIZE> shape, enum device dev, 
                T tol, T s, enum mgard_cuda::error_bound_type mode, T norm, 
                T * original_data, 
                unsigned char * &compressed_data, size_t &compressed_size, mgard_cuda::Config config) {
  std::cout << mgard_cuda::log::log_info << "Start compressing\n";
  high_resolution_clock::time_point t1, t2;
  duration<double> time_span;
  std::array<std::size_t, D> array_shape;
  std::copy(shape.begin(), shape.end(), array_shape.begin());
  if (dev == CPU) {
    if (mode == mgard_cuda::REL) tol *= norm;
    const mgard::TensorMeshHierarchy<D, T> hierarchy(array_shape);
    mgard::CompressedDataset<D, T> compressed_dataset = mgard::compress(hierarchy, original_data, s, tol);
    compressed_size = compressed_dataset.size();
    compressed_data = (unsigned char *)malloc(compressed_size);
    memcpy(compressed_data, compressed_dataset.data(), compressed_size);
  } else {
    mgard_cuda::Array<D, T> in_array(shape);
    in_array.loadData(original_data);
    mgard_cuda::Handle<D, T> handle(shape, config);
    if (handle.arch == 1) std::cout << mgard_cuda::log::log_info << "Optimized for Volta.\n";
    if (handle.arch == 2) std::cout << mgard_cuda::log::log_info << "Optimized for Turing.\n";
    t1 = high_resolution_clock::now();
    mgard_cuda::Array<1, unsigned char> compressed_array =
        mgard_cuda::compress(handle, in_array, mode, tol, s);
    t2 = high_resolution_clock::now();
    time_span = duration_cast<duration<double>>(t2 - t1);
    std::cout << mgard_cuda::log::log_time << "Compression API time: "<< time_span.count() << " s (" << 
            (double)(handle.dofs[0][0] * handle.dofs[1][0] *handle.linearized_depth
            *sizeof(T))/time_span.count()/1e9 << " GB/s)\n";
    compressed_size = compressed_array.getShape()[0];
    compressed_data = (unsigned char *)malloc(compressed_size);
    memcpy(compressed_data, compressed_array.getDataHost(),
           compressed_size);  
  }
}


template <mgard_cuda::DIM D, typename T>
void decompression(std::vector<mgard_cuda::SIZE> shape, enum device dev, 
                T tol, T s, enum mgard_cuda::error_bound_type mode, T norm, 
                unsigned char * compressed_data, size_t compressed_size,
                T *& decompressed_data, mgard_cuda::Config config) {

  std::cout << mgard_cuda::log::log_info << "Start decompressing\n";
  high_resolution_clock::time_point t1, t2;
  duration<double> time_span;
  size_t original_size = 1;
  for (int i = 0; i < D; i++) original_size *= shape[i];
  decompressed_data = (T*)malloc(original_size * sizeof(T));
  if (dev == CPU) {
    if (mode == mgard_cuda::REL) tol *= norm;
    std::array<std::size_t, D> array_shape;
    std::copy(shape.begin(), shape.end(), array_shape.begin());
    const mgard::TensorMeshHierarchy<D, T> hierarchy(array_shape);
    mgard::CompressedDataset<D, T> compressed_dataset(hierarchy, s,
                    tol, compressed_data, compressed_size);
    mgard::DecompressedDataset<D, T> decompressed_dataset = mgard::decompress(compressed_dataset);
    memcpy(decompressed_data, decompressed_dataset.data(), original_size * sizeof(T));
  } else { // GPU
    mgard_cuda::Handle<D, T> handle(shape, config);
    if (handle.arch == 1) std::cout << mgard_cuda::log::log_info << "Optimized for Volta.\n";
    if (handle.arch == 2) std::cout << mgard_cuda::log::log_info << "Optimized for Turing.\n";
    std::vector<mgard_cuda::SIZE> compressed_shape(1);
    compressed_shape[0] = compressed_size;
    mgard_cuda::Array<1, unsigned char> compressed_array (compressed_shape);
    compressed_array.loadData(compressed_data);
    t1 = high_resolution_clock::now(); 
    mgard_cuda::Array<D, T> out_array =
        mgard_cuda::decompress(handle, compressed_array);
    t2 = high_resolution_clock::now();
    time_span = duration_cast<duration<double>>(t2 - t1);
    std::cout << mgard_cuda::log::log_time << "Decompression API time: "<< time_span.count() << " s (" << 
            (double)(handle.dofs[0][0] * handle.dofs[1][0] *handle.linearized_depth
            *sizeof(T))/time_span.count()/1e9 << " GB/s)\n";
    memcpy(decompressed_data, out_array.getDataHost(),
           original_size * sizeof(T));
  }
}

template <typename T>
int test(int D, char * input_file, enum data_type dtype, std::vector<mgard_cuda::SIZE> shape, enum device dev, double tol, double s, enum mgard_cuda::error_bound_type mode) {

  size_t original_size = 1;
  for (int i = 0; i < D; i++) original_size *= shape[i];
  T * original_data = (T*)malloc(original_size * sizeof(T));
  readfile(input_file, original_size * sizeof(T), false, original_data);

  T norm;
  if (s == std::numeric_limits<T>::infinity()) {
    norm = mgard_cuda::L_inf_norm(original_size, original_data);
  } else {
    norm = mgard_cuda::L_2_norm(original_size, original_data);
  }

  mgard_cuda::Config config;
  config.gpu_lossless = true;
  config.huff_dict_size = 8192;
  config.huff_block_size = 1024 * 30;
  config.enable_lz4 = false;
  config.lz4_block_size = 1 << 15;
  config.reduce_memory_footprint = true;
  config.sync_and_check_all_kernels = true;
  config.timing = true;

  unsigned char * compressed_data;
  size_t compressed_size;
  T * decompressed_data;
  if (D == 1) {
    compression<1, T>(shape, dev, tol, s, mode, norm, original_data, 
                compressed_data, compressed_size, config);
    decompression<1, T>(shape, dev, tol, s, mode, norm, 
                compressed_data, compressed_size, decompressed_data, config);
  }
  if (D == 2) {
    compression<2, T>(shape, dev, tol, s, mode, norm, original_data, 
                compressed_data, compressed_size, config);
    decompression<2, T>(shape, dev, tol, s, mode, norm, 
                compressed_data, compressed_size, decompressed_data, config);
  }
  if (D == 3) {
    compression<3, T>(shape, dev, tol, s, mode, norm, original_data, 
                compressed_data, compressed_size, config);
    decompression<3, T>(shape, dev, tol, s, mode, norm, 
                compressed_data, compressed_size, decompressed_data, config);
  }
  if (D == 4) {
    compression<4, T>(shape, dev, tol, s, mode, norm, original_data, 
                compressed_data, compressed_size, config);
    decompression<4, T>(shape, dev, tol, s, mode, norm, 
                compressed_data, compressed_size, decompressed_data, config);
  }
  if (D == 5) {
    compression<5, T>(shape, dev, tol, s, mode, norm, original_data, 
                compressed_data, compressed_size, config);
    decompression<5, T>(shape, dev, tol, s, mode, norm, 
                compressed_data, compressed_size, decompressed_data, config);
  }

  // printf("org: ");
  // for (int i = 0; i < original_size; i++) printf("%f ", original_data[i]);
  // printf("\n");
  // printf("decomp: ");
  // for (int i = 0; i < original_size; i++) printf("%f ", decompressed_data[i]);
  // printf("\n");


  printf("In size:  %10ld  Out size: %10ld  Compression ratio: %10ld \n", original_size * sizeof(T),
         compressed_size, original_size * sizeof(T) / compressed_size);

  T error;
  if (s == std::numeric_limits<T>::infinity()) {
    error = mgard_cuda::L_inf_error(original_size, original_data, decompressed_data);
    if (mode ==  mgard_cuda::REL) { error /= norm; printf("Rel. L^infty error: %10.5E \n", error); }
    if (mode ==  mgard_cuda::ABS) printf("Abs. L^infty error: %10.5E \n", error);
  } else {
    error = mgard_cuda::L_2_error(original_size, original_data, decompressed_data);
    if (mode ==  mgard_cuda::REL) { error /= norm; printf("Rel. L^2 error: %10.5E \n", error); }
    if (mode ==  mgard_cuda::ABS) printf("Abs. L^2 error: %10.5E \n", error);
  }

  if (error < tol) {
    printf(ANSI_GREEN "SUCCESS: Error tolerance met!" ANSI_RESET "\n");
    return 0;
  } else {
    printf(ANSI_RED "FAILURE: Error tolerance NOT met!" ANSI_RESET "\n");
    return -1;
  }
}

int main(int argc, char *argv[]) {
  size_t result;

  if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
    print_usage_message(argv, stdout);
  }

  int i = 1;

  char *input_file;    //, *outfile;
  input_file = argv[i++];
  
  enum data_type dtype;
  char * dt = argv[i++];
  if (strcmp(dt, "s") == 0) dtype = SINGLE;
  else if (strcmp(dt, "d") == 0) dtype = DOUBLE;
  else print_usage_message(argv, stdout);

  std::vector<mgard_cuda::SIZE> shape;
  int D = atoi(argv[i++]);
  for (mgard_cuda::DIM d = 0; d < D; d++) {
    shape.push_back(atoi(argv[i++]));
  }


  enum mgard_cuda::error_bound_type mode; // REL or ABS
  char * em = argv[i++];
  if (strcmp(em, "rel") == 0) { mode = mgard_cuda::REL; }
  else if (strcmp(em, "abs") == 0) { mode = mgard_cuda::ABS; }
  else print_usage_message(argv, stdout);

  double tol, s = 0;
  tol = atof(argv[i++]);
  s = atof(argv[i++]);

  enum device dev; // CPU or GPU
  char * d = argv[i++];
  if (strcmp(d, "cpu") == 0) dev = CPU;
  else if (strcmp(d, "gpu") == 0) dev = GPU;
  else print_usage_message(argv, stdout);

  print_config(input_file, dtype, shape, dev, tol, s, mode); 

  if (dtype == SINGLE) {
    return test<float>(D, input_file, dtype, shape, dev, tol, s, mode);
  } else if (dtype == DOUBLE) {
    return test<double>(D, input_file, dtype, shape, dev, tol, s, mode);
  }



 

  // printf("In size:  %10ld  Out size: %10ld  Compression ratio: %10ld \n", lSize,
  //        out_size, lSize / out_size);

  // // FILE *qfile;
  // // qfile = fopen ( outfile , "wb" );
  // // result = fwrite (mgard_out_buff, 1, lSize, qfile);
  // // fclose(qfile);
  // // if (result != lSize) {fputs ("Writing error",stderr); exit (4);}
  // int error_count = 100;
  // double error_L_inf_norm = 0;
  // for (int i = 0; i < num_double; ++i) {
  //   double temp = fabs(in_buff[i] - mgard_out_buff[i]);
  //   if (temp > error_L_inf_norm)
  //     error_L_inf_norm = temp;
  //   if (temp / data_L_inf_norm >= tol && error_count) {
  //     // printf("not bounded: buffer[%d]: %f vs. mgard_out_buff[%d]: %f \n", i,
  //     //        in_buff[i], i, mgard_out_buff[i]);
  //     error_count--;
  //   }
  // }

  // double max = 0, min = std::numeric_limits<double>::max(), range = 0;
  // double error_sum = 0, mse = 0, psnr = 0;
  // for (int i = 0; i < num_double; ++i) {
  //   if (max < in_buff[i]) max = in_buff[i];
  //   if (min > in_buff[i]) min = in_buff[i];
  //   double err = fabs(in_buff[i] - mgard_out_buff[i]);
  //   error_sum += err * err;
  // }
  // range = max - min;
  // mse = error_sum / num_double;
  // psnr = 20*log::log10(range)-10*log::log10(mse);
  
  // mgard_cuda::cudaFreeHostHelper(in_buff);

  // // printf("sum: %e\n", sum/num_double);
  // double relative_L_inf_error = error_L_inf_norm / data_L_inf_norm;

  // // std::ofstream fout("mgard_out.dat", std::ios::binary);
  // // fout.write(reinterpret_cast<const char *>(mgard_comp_buff), out_size);
  // // fout.close();




  // printf("Rel. L^infty error bound: %10.5E \n", tol);
  // printf("Rel. L^infty error: %10.5E \n", relative_L_inf_error);
  // printf("MSE: %10.5E\n", mse);
  // printf("PSNR: %10.5E\n", psnr);

  // if (relative_L_inf_error < tol) {
  //   printf(ANSI_GREEN "SUCCESS: Error tolerance met!" ANSI_RESET "\n");
  //   return 0;
  // } else {
  //   printf(ANSI_RED "FAILURE: Error tolerance NOT met!" ANSI_RESET "\n");
  //   return -1;
  // }
  // delete [] mgard_out_buff;
}
