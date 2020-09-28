#pragma once

#include <cmath>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

/// config encapsulates all of the configuration behaviors that we require of
/// our benchmarks.  It standardizes the format of command-line arguments,
/// parsing of command-line arguments, and reporting of command-line arguments.
///
/// config is a superset of everything that our individual benchmarks need.  For
/// example, it has a chunk　size, even though our linked list benchmark doesn't
/// need it.  The price of such generality is small, and the code savings is
/// large.
///
/// The purpose of config is not to hide information, but to reduce boilerplate
/// code.  We aren't concerned about good object-oriented design, so everything
/// is public.
struct config {
  /// Interval of time the test should run for in seconds
  int interval = 5;

  /// Our benchmark harness uses the data structures as integer sets or
  /// integer-integer maps.  This is the range for keys in the maps, and for
  /// elements in the sets.
  size_t key_range = 65536;

  /// Number of threads that should execute the benchmark code
  int nthreads = 1;

  /// Lookup ratio.  The remaining　elemental operations will split evenly
  /// between inserts and removes
  int lookup = 80;

  /// The size of index layer vectors.
  int index_size = 0;

  /// If the data structure uses chunks, this is the size of each chunk
  int chunksize = 32;

  /// This is the proportion of threads that will be dedicated to traversals,
  /// represented as a percentage. This fraction will be rounded up to the next
  /// whole number of threads.
  /// If set to a negative number, uses exactly as many threads as the absolute
  /// value.
  float traversal_pctg = 0.0;

  /// The percentage of traversals that are ragne() operations, for benchmarks
  /// that have range queries.
  /// This uses an RNG out of 100, so float values are not allowed.
  int range_pctg = 0;

  /// Percentage of foreach() and range() operations that will exit early.
  /// This uses an RNG out of 100, so float values are not allowed.
  int early_exit_pctg = 0;

  /// Percentage of foreach() and range() operations that will be read-only.
  /// This uses an RNG out of 100, so float values are not allowed.
  int readonly_traversal_pctg = 0;

  /// Should the benchmark output a lot of data, or just a little?
  bool verbose = false;

  /// Should the benchmark forgo pretty printing entirely, and output as a
  /// comma-separated list for the sake of data processing?
  bool output_raw = false;

  /// The merge threshold of a skipvector
  float merge_threshold = 2.0;

  /// Number of index layers (skipvector only).
  /// 0 index layers is not allowed. There is always at least one index layer.
  /// 0 is a special value that automatically computes the ideal number of
  /// layers from the key range and vector sizes.
  int layers = 0;

  /// The difference between start and end keys in a range query... We will
  /// randomly choose a start, and then end will be this far away from it.
  /// Note: 0 is a special value indicating that, rather than using a fixed
  /// range size, we choose a start and an end point uniformly at random, such
  /// that end >= start, allowing variable range lengths.
  size_t range_dist = 0;

  /// The name of the specific data structure to test
  std::string data_structure_name;

  /// The name of the specific data structure variant to test
  std::string bench_name = "hp";

  /// The name of the executable
  std::string program_name;

  /// A description of the program
  std::string bench_description;

  /// All of the possible data structure implementations that can be run
  std::vector<std::string> ds_options;

  /// A statement about any command-line options that don't pertain to a
  /// particular program
  std::string unused_options_statement;

  /// Initialize the program's configuration by setting the strings that are not
  /// dependent on the command-line
  config(const std::string &prog_name, const std::string &bench_desc,
         const std::vector<std::string> &ds_opts,
         const std::string &unused_stmt)
      : program_name(prog_name), bench_description(bench_desc),
        ds_options(ds_opts), unused_options_statement(unused_stmt) {}

  /// Usage() reports on the command-line options for the benchmark
  void usage() {
    using std::cout;
    using std::endl;

    cout << program_name << ":" << bench_description << endl
         << " -b: benchmark                                   (default hp)"
         << endl
         << " -c: vector capacity                             (default 32)"
         << endl
         << "  (0: special value that means skiplist simulation mode)    "
         << endl
         << " -d: merge threshold                             (default 2.0)"
         << endl
         << " -e: percentage of traversals that exit early    (default 0.0%)"
         << endl
         << " -f: percentage of threads doing traversals      (default 0%)"
         << endl
         << "  (if negative: special value, use exactly |n| threads)    "
         << endl
         << " -g: range() ops (as a percentage of traversals) (default 0%)"
         << endl
         << " -h: print this message                          (default false)"
         << endl
         << " -i: test interval in seconds                    (default 5)"
         << endl
         << " -k: key range                                   (default 65536)"
         << endl
         << " -l: lookups (as a percentage of elementals)     (default 80%)"
         << endl
         << " -o: output raw (CSV)                            (default false)"
         << endl
         << " -r: percentage of traversals that are read-only (default 0%)"
         << endl
         << " -s: index layer node capacity                   (default: 0)"
         << endl
         << "  (0: special value that means skiplist simulation mode)    "
         << endl
         << "  (-1: special value that sets -s to the same as -c)        "
         << endl
         << " -t: # threads                                   (default 1)"
         << endl
         << " -v: be verbose?                                 (default false)"
         << endl
         << " -w: window size for HTM                         (default 16)"
         << endl
         << " -x: number of index layers                      (default 0)"
         << endl
         << "  (0: automatically calculate ideal value)                  "
         << endl
         << " -z: distance of range()                         (default 0)"
         << endl
         << "  (0: special value that means choose range uniformly at random)"
         << endl
         << "      [ ";
    for (auto i : ds_options) {
      cout << i << " ";
    }
    cout << "]" << endl;
    if (unused_options_statement != "") {
      cout << unused_options_statement << endl;
    }
  }

  /// Parse the command-line options to initialize fields of the config object
  void init_from_args(const std::string &ds, int argc, char **argv) {
    using std::stof;
    using std::stoi;
    using std::stoull;
    using std::string;

    data_structure_name = ds;
    long opt;
    while ((opt = getopt(argc, argv, "b:c:d:e:f:g:hi:k:l:or:s:t:vx:z:")) !=
           -1) {
      switch (opt) {
      case 'b':
        bench_name = std::string(optarg);
        break;
      case 'c':
        chunksize = stoi(optarg);
        break;
      case 'd':
        merge_threshold = stof(optarg);
        break;
      case 'e':
        early_exit_pctg = stoi(optarg);
        break;
      case 'f':
        traversal_pctg = stof(optarg);
        break;
      case 'g':
        range_pctg = stoi(optarg);
        break;
      case 'h':
        usage();
        exit(0);
      case 'i':
        interval = stoi(optarg);
        break;
      case 'k':
        key_range = stoull(optarg);
        break;
      case 'l':
        lookup = stoi(optarg);
        break;
      case 'o':
        output_raw = !output_raw;
        break;
      case 'r':
        readonly_traversal_pctg = stoi(optarg);
        break;
      case 's':
        index_size = stoi(optarg);
        break;
      case 't':
        nthreads = stoi(optarg);
        break;
      case 'v':
        verbose = !verbose;
        break;
      case 'x':
        layers = stoi(optarg);
        break;
      case 'z':
        range_dist = stoull(optarg);
        break;
      }
    }

    // If index layer vector size is set to special value -1,
    // set it to the same as the data layer size.
    if (index_size == -1 && ds == "skipvector") {
      index_size = chunksize;
    }

    // If layers is set to the special value 0, automatically compute ideal
    // value: ceil(log base s of (e/c)), where e is the number of elements.
    if (layers == 0) {
      double data_node_count_target = key_range * 0.5 / chunksize;

      layers = static_cast<int>(
          std::ceil(std::log(data_node_count_target) / std::log(index_size)));

      // Set a minimum of one
      layers = layers < 1 ? 1 : layers;
    }
  }

  /// Report the current values of the configuration object
  void report() {
    using std::cout;
    using std::endl;

    if (output_raw)
      return;

#ifndef NDEBUG
    // Clearly mark any results where NDEBUG not set.
    cout << "NDEBUG is not set, so certain implementations may run slower! "
            "Make sure to #define NDEBUG if you want optimal performance!"
         << endl;
#endif

    cout << "configuration ";
    print_config(std::string(", "));
    cout << endl;
  }

  /// Report the current values of the configuration object as a comma-separated
  /// line
  void report_raw() {
    using std::cout;

#ifndef NDEBUG
    // Clearly mark any results where NDEBUG not set.
    cout << "DEBUG-";
#endif

    cout << data_structure_name << ",";
    print_config(std::string(","));
  }

  /// s is the separator
  void print_config(const std::string &s) {
    using std::cout;

    // Legend, -bc
    cout << "(bcdefgiklrstxz)" << s << bench_name << s << chunksize << s
         << merge_threshold << s << early_exit_pctg << s << traversal_pctg << s
         << range_pctg << s << interval << s << key_range << s << lookup << s
         << readonly_traversal_pctg << s << index_size << s << nthreads << s
         << layers << s << range_dist;
  }
};
