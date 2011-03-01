#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <iostream>


#include <graphlab.hpp>
#include <graphlab/rpc/dc.hpp>
#include <graphlab/rpc/dc_init_from_env.hpp>
#include <graphlab/rpc/dc_init_from_mpi.hpp>
#include <graphlab/logger/logger.hpp>
#include <graphlab/serialization/serialization_includes.hpp>
#include <graphlab/distributed2/graph/distributed_graph.hpp>
#include <graphlab/distributed2/distributed_chromatic_engine.hpp>
#include <graphlab/distributed2/distributed_glshared.hpp>
using namespace graphlab;


#include <graphlab/macros_def.hpp>

/// GLOBAL CONSTANTS
const size_t MAX_CHANGES(10);
const size_t MAX_ITERATIONS(1000);
const size_t SYNC_INTERVAL(100);
const size_t NUM_COLORS(10);

///////////////////////////////////////////////////////////////////////////////
///////////////////////// Types ////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
struct vertex_data_type {
  procid_t atomid;
  procid_t num_changes;
  bool       is_set;
  bool       is_seed;
};
SERIALIZABLE_POD(vertex_data_type);
struct edge_data_type { };
SERIALIZABLE_POD(edge_data_type);


typedef distributed_graph<vertex_data_type, edge_data_type> graph_type;
typedef distributed_chromatic_engine< graph_type > engine_type;
typedef engine_type::iscope_type iscope_type;
typedef engine_type::icallback_type icallback_type;
typedef ishared_data<graph_type> ishared_data_type;
typedef engine_type::icallback_type icallback_type;
typedef engine_type::update_task_type update_task_type;


size_t num_atoms(10);

struct statistics {
  typedef std::map<procid_t, double> atom2count_type;

  atom2count_type atom2count;
  size_t num_unset;
  size_t edge_cut;
  size_t visited;
  std::vector<vertex_id_t> nextvset;
  statistics() : num_unset(0), edge_cut(0), visited(0) { }
  statistics(size_t num_atoms) : 
    num_unset(0),  edge_cut(0), visited(0),
    nextvset(num_atoms, vertex_id_t(-1)) { }
  
  void operator+=(const iscope_type& iscope) {
    visited++;
    const vertex_data_type& vdata(iscope.const_vertex_data());
    if(vdata.is_set)  {
      atom2count[vdata.atomid]++;
      foreach(const edge_id_t eid, iscope.in_edge_ids()) {
        const vertex_id_t nvid(iscope.source(eid));
        const vertex_data_type& nvdata = 
          iscope.const_neighbor_vertex_data(nvid);
        if(nvdata.is_set && nvdata.atomid != vdata.atomid)
          edge_cut++;
      } // end of loop over edges
    } else {
      if(num_unset < nextvset.size()) {
        nextvset[num_unset] = iscope.vertex();
      } else if( random::rand01() < double(nextvset.size())/num_unset) {
        nextvset[rand() % nextvset.size()] = iscope.vertex();
      }
      num_unset++;
    } 
  }
  void operator+=(const statistics& other) {
    typedef atom2count_type::value_type pair_type;
    edge_cut += other.edge_cut;
    visited += other.visited;
    foreach(const pair_type& pair, other.atom2count)
      atom2count[pair.first] += pair.second;
    std::vector<vertex_id_t> this_nextvset(nextvset);
    const size_t num_atoms(nextvset.size());
    ASSERT_EQ(other.nextvset.size(), num_atoms);
    // fill out the rest of this vector
    size_t i(0), j(0), k(0);
    while(i <  num_atoms && j < num_unset && k < other.num_unset) {
      const double accept_prob = 
        double(other.num_unset - k) / 
        double((other.num_unset + num_unset) - (j+k));
      ASSERT_GE(accept_prob, 0);
      if(random::rand01() < accept_prob) 
        nextvset[i++] = other.nextvset[k++];
      else nextvset[i++] = this_nextvset[j++];      
    }
    while(i <  num_atoms && j < num_unset) 
      nextvset[i++] = this_nextvset[j++];      
    while(i <  num_atoms && k < other.num_unset) 
      nextvset[i++] = other.nextvset[k++];      
    num_unset += other.num_unset;
    for(size_t u = 0; u < nextvset.size() && u < num_unset; ++u) 
      ASSERT_NE(nextvset[u], vertex_id_t(-1));
  }
  void print() {
    std::cout 
      << "------------------------------------------------------------\n";
    std::cout << "Visited: " << visited << std::endl;
    std::cout << "Vertex Bal: " << vertex_balance() << std::endl;
    std::cout << "Edge cut: " << edge_cut << std::endl;
    typedef atom2count_type::value_type pair_type;
    foreach(const pair_type& pair, atom2count) 
      std::cout << "(" << pair.first << ", " << pair.second << ")  ";
    std::cout << "\n";
    std::cout 
      << "------------------------------------------------------------"
      << std::endl;      
  }
  
  double vertex_balance() {
    double max_count(0);
    typedef atom2count_type::value_type pair_type;
    foreach(pair_type& pair, atom2count) 
      max_count = std::max(max_count, pair.second);
    return max_count * atom2count.size();
  }

  void finalize() {
    double sum(0); 
    typedef atom2count_type::value_type pair_type;
    foreach(pair_type& pair, atom2count) sum += pair.second;
    ASSERT_GT(sum, 0);
    foreach(pair_type& pair, atom2count) pair.second /= sum;   
    print(); 
  }   
  void load(iarchive& iarc) {
    iarc >> atom2count >> num_unset >> nextvset 
         >> edge_cut >> visited;
  }
  void save(oarchive& oarc) const {
    oarc << atom2count << num_unset << nextvset
         << edge_cut << visited;
  }
};

typedef distributed_glshared<statistics> shared_statistics_type;
// global data
shared_statistics_type shared_statistics;

// update the counts in the appropriate table
void statistics_sum_fun(iscope_type& iscope,  any& acc) {
  acc.as<statistics>() += iscope;
}
// Identity apply
void statistics_apply_fun(any& current_data, 
                           const any& acc) { 
  current_data.as<statistics>() = acc.as<statistics>();
  current_data.as<statistics>().finalize();
} 
// Sum the two maps
void statistics_merge_fun(any& any_dest,  const any& any_src) {
  any_dest.as<statistics>() += any_src.as<statistics>();
}










procid_t find_best_atom(statistics::atom2count_type& local_atom2count,
                        const statistics::atom2count_type& global_atom2count) {
  double best_score = 0;
  procid_t best_atomid = local_atom2count.begin()->first;
  ASSERT_LT(best_atomid, num_atoms);
  typedef statistics::atom2count_type::value_type pair_type;
  // normalize the local_atom2count map
  double sum(0);
  foreach(pair_type& pair, local_atom2count) sum += pair.second;
  ASSERT_GT(sum, 0);
  foreach(pair_type& pair, local_atom2count) pair.second /= sum;

  foreach(const pair_type& pair, local_atom2count) {
    const procid_t atomid(pair.first);
    ASSERT_LT(atomid, num_atoms);
    const double local_count(pair.second);
    ASSERT_GT(local_count, 0);
    const double global_count(safe_get(global_atom2count, atomid, double(0)) );
    // auto join
    if(global_count == 0) { return atomid; }
    ASSERT_GT(global_count, 0);
    // otherwise compute the 
    const double score = local_count / global_count;
    if(score > best_score)  {
      best_atomid = atomid;
      best_score = score;
    }
  }
  return best_atomid;
} // end of best join atoms








void partition_update_function(iscope_type& scope,
                               icallback_type& callback,
                               ishared_data_type* unused) {
  statistics::atom2count_type local_atom2count;
  // Get the number of neighbor assignments
  foreach(const edge_id_t eid, scope.in_edge_ids()) {
    const vertex_id_t vid(scope.source(eid));
    const vertex_data_type& vdata = 
      scope.const_neighbor_vertex_data(vid);
    if(vdata.is_set) ++local_atom2count[vdata.atomid];
  }
  foreach(const edge_id_t eid, scope.out_edge_ids()) {
    const vertex_id_t vid(scope.target(eid));
    const vertex_data_type& vdata = 
      scope.const_neighbor_vertex_data(vid);
    if(vdata.is_set) ++local_atom2count[vdata.atomid];
  }

  // Get the vertex data
  const vertex_data_type& vdata(scope.const_vertex_data());


  // If the neighbor change has not reached this machine yet then
  // reschedule self
  if(!vdata.is_seed && local_atom2count.empty()) {
    callback.add_task(scope.vertex(), partition_update_function);
    return;
  }

  bool changed(false);
  if(!vdata.is_seed) {
    ASSERT_GT(local_atom2count.size(), 0);
    // Get the new atomid assignment for this vertex
    typedef shared_statistics_type::const_ptr_type shared_ptr_type;
    shared_ptr_type shared_statistics_ptr(shared_statistics.get_ptr());
    const procid_t new_atomid = 
      find_best_atom(local_atom2count, shared_statistics_ptr->atom2count);

    if(!vdata.is_set ||
       (vdata.num_changes < MAX_CHANGES && 
        vdata.atomid != new_atomid) ) {
      vertex_data_type& vdata(scope.vertex_data());
      vdata.atomid = new_atomid;
      vdata.is_set = true;
      vdata.num_changes++;
      changed = true;
    }
  } // end update assig
  // Reschedule the neighbors
  if(changed || vdata.is_seed) {
    // Schedule all in neighbors
    foreach(const edge_id_t eid, scope.in_edge_ids()) {
      const vertex_id_t vid(scope.source(eid));
      const vertex_data_type& vdata = 
        scope.const_neighbor_vertex_data(vid);
      if(vdata.num_changes < MAX_CHANGES) 
        callback.add_task(vid, partition_update_function);
    }
    // Schedule all out neighbors
    foreach(const edge_id_t eid, scope.out_edge_ids()) {
      const vertex_id_t vid(scope.target(eid));
      const vertex_data_type& vdata = 
        scope.const_neighbor_vertex_data(vid);
      if(vdata.num_changes < MAX_CHANGES) 
        callback.add_task(vid, partition_update_function);
    }
  }
} // end of partition_update_function










int main(int argc, char** argv) {
  // set the global logger
  global_logger().set_log_level(LOG_INFO);
  global_logger().set_log_to_console(true);
  // Initialize the mpi tools
  graphlab::mpi_tools::init(argc, argv);

  // Parse the command lines
  std::string aindex("atom_index.txt");
  std::string partfile("partitioning.txt");
  graphlab::command_line_options 
    clopts("Partition the graph using the GraphLab engine.");
  clopts.attach_option("aindex", &aindex, aindex,
                       "The atom index file.");
  clopts.attach_option("nparts", &num_atoms, num_atoms,
                       "The number of parts to create.");
  clopts.attach_option("partfile", &partfile, partfile,
                       "[output] file containing the partitioning.");
  if( !clopts.parse(argc, argv) ) { 
    std::cout << "Error parsing command line arguments!"
              << std::endl;
    return EXIT_FAILURE;
  }
  logstream(LOG_INFO) << "Partitioning into " << num_atoms
                      << " parts." << std::endl;

  // Initialize the distributed control plane
  dc_init_param param;
  if( ! init_param_from_mpi(param) ) 
    logstream(LOG_FATAL) 
      << "Failed MPI laucher!" << std::endl; 
  param.initstring = "buffered_queued_send=yes, ";
  param.numhandlerthreads = 8;
  global_logger().set_log_level(LOG_DEBUG);
  distributed_control dc(param);


  logstream(LOG_INFO) 
    << "Loading graph from atom index file: " << aindex << std::endl;
  const bool NO_LOAD_DATA(true);
  graph_type  graph(dc, aindex, NO_LOAD_DATA);
 

  logstream(LOG_INFO)
    << "Artificially color the graph" << std::endl;
  foreach(const vertex_id_t vid, graph.owned_vertices()) {
    graph.color(vid) =  rand() % NUM_COLORS;
  }


  logstream(LOG_INFO)  
    << "Initializing engine with " << clopts.get_ncpus() 
    << " local threads." <<std::endl;
  engine_type engine(dc, graph, clopts.get_ncpus());


  logstream(LOG_INFO)  
    << "Set the scheduler options." << std::endl;  
  scheduler_options schedopts;
  schedopts.add_option("update_function", partition_update_function);
  //  schedopts.add_option("max_iterations", MAX_ITERATIONS);
  engine.set_scheduler_options(schedopts);

  logstream(LOG_INFO) << "Register a sync." << std::endl;
  engine.set_sync(shared_statistics,
                  statistics_sum_fun,
                  statistics_apply_fun,
                  any(statistics(num_atoms)), 
                  SYNC_INTERVAL,
                  statistics_merge_fun);

  
  logstream(LOG_INFO) << "Scheduling tasks." << std::endl;  

  // Scheduling tasks
  if(dc.procid() == 0) {
    for(size_t i = 0; i < num_atoms; ++i) {
      const vertex_id_t vid(rand() % graph.num_vertices());
      vertex_data_type vdata;
      vdata.atomid = i;
      vdata.is_set = true;  
      vdata.is_seed = true;
      graph.set_vertex_data(vid, vdata);
      logstream(LOG_INFO) << "Adding seed: " << vid << std::endl;
      engine.add_vtask(vid, partition_update_function);
    }
  }

  logstream(LOG_INFO) << "Running partitioner." << std::endl;
  size_t iteration_counter(0);
  for(; true; iteration_counter++) {
    std::cout << "Starting iteration: " << iteration_counter
              << std::endl;        
    engine.start();
    statistics stats(shared_statistics.get_val());    
    std::cout << "Finished iteration: " << iteration_counter
              << std::endl;      
    if(stats.num_unset == 0) break;
    // Gather unset vertices
    if(dc.procid() == 0) {
      stats.print();
      ASSERT_EQ(stats.nextvset.size(), num_atoms);     
      std::cout << "Num unset: " << stats.num_unset << std::endl;
      for(size_t i = 0; i < num_atoms && i < stats.num_unset; ++i) {
        ASSERT_NE(stats.nextvset[i], vertex_id_t(-1));
        const vertex_id_t vid(stats.nextvset[i]);
        vertex_data_type vdata;
        vdata.atomid = i;
        vdata.is_set = true;  
        vdata.is_seed = true;
        graph.set_vertex_data(vid, vdata);
        logstream(LOG_INFO) << "Adding seed: " << vid << std::endl;
        engine.add_vtask(vid, partition_update_function);
      }
    } // end of if 0
  } // end of loop for loop
  logstream(LOG_INFO) << "Finished partitioning." << std::endl;
 

 
  logstream(LOG_INFO) << "Gathering partitioning." << std::endl;

  typedef std::vector< std::pair<vertex_id_t, procid_t> > vector_of_pairs;
  std::vector<vector_of_pairs> proc2pairs(dc.numprocs());
  foreach(const vertex_id_t vid, graph.owned_vertices()) {
    const vertex_data_type& vdata(graph.vertex_data(vid));
    // Require all vertices to be assinged a class
    ASSERT_TRUE(vdata.is_set);
    proc2pairs[dc.procid()].
      push_back(std::make_pair(vid, vdata.atomid));
  }
  const size_t ROOT_NODE(0);
  dc.gather(proc2pairs, ROOT_NODE);
  if (dc.procid() == ROOT_NODE) {
    // construct final map
    std::vector<procid_t> result(graph.num_vertices());
    std::vector<size_t> counts(num_atoms);
    std::vector<size_t> vertex2proc(graph.num_vertices());
    for (size_t i = 0; i < dc.numprocs(); ++i) {
      for(size_t j = 0; j < proc2pairs[i].size(); ++j) {
        result.at(proc2pairs[i][j].first) = proc2pairs[i][j].second;
        counts.at(proc2pairs[i][j].second)++;
        vertex2proc.at(proc2pairs[i][j].first) = i;
      }
    }
    {
      std::ofstream fout(partfile.c_str());
      ASSERT_TRUE(fout.good());
      for(size_t i = 0; i < result.size(); ++i) 
        fout << result[i] << "\n";    
      fout.close();
    }
    {
      std::string fname = "machine_" + partfile;
      std::ofstream fout(fname.c_str());
      ASSERT_TRUE(fout.good());
      for(size_t i = 0; i < result.size(); ++i) 
        fout << vertex2proc[i] << "\n";    
      fout.close();
    }

    
    std::cout << "\n\n\n\n" << std::endl 
              <<  "======================================"
              << "\n\n" << std::endl;

    std::cout << "Counts:  ";
    size_t max_counts(0);
    for(size_t i = 0; i < counts.size(); ++i) {
      std::cout << counts[i]  << '\t';
      max_counts = std::max(max_counts, counts[i]);
    }
    std::cout << std::endl;

    std::cout << "ECounts: ";
    statistics  stats(shared_statistics.get_val());
    typedef statistics::atom2count_type::value_type pair_type;
    foreach(pair_type pair, stats.atom2count) 
      std::cout << pair.second  << '\t';
    std::cout << std::endl;

    const double imbalance = 
      double(max_counts) * double(counts.size()) / 
      double(graph.num_vertices());
    std::cout << "Imbalance max/average: " << imbalance << std::endl;

    std::cout << "\n\n" << std::endl 
              <<  "======================================"
              << "\n\n\n\n" << std::endl;




  }
 
  // Gather metrics
  dc.fill_metrics();
  graph.fill_metrics();

  
  if(dc.procid() == ROOT_NODE) {
    basic_reporter reporter;
    metrics::report_all(reporter);
  } 
  
  logstream(LOG_INFO) << "Finished " << dc.procid() << std::endl;
  dc.full_barrier();
  graphlab::mpi_tools::finalize();  
  return EXIT_SUCCESS;
}

