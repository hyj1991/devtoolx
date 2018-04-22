#include "snapshot_parser.h"

namespace snapshot_parser {
SnapshotParser::SnapshotParser(json profile) {
  nodes = profile["nodes"];
  edges = profile["edges"];
  strings = profile["strings"];
  snapshot = profile["snapshot"];
  if (snapshot["root_index"] != nullptr) {
    root_index = snapshot["root_index"];
  }
  json node_fields = snapshot["meta"]["node_fields"];
  json edge_fields = snapshot["meta"]["edge_fields"];
  node_field_length = node_fields.size();
  edge_field_length = edge_fields.size();
  node_count = nodes.size() / node_field_length;
  edge_count = edges.size() / edge_field_length;
  node_types = snapshot["meta"]["node_types"][0];
  edge_types = snapshot["meta"]["edge_types"][0];
  node_type_offset = IndexOf(node_fields, "type");
  node_name_offset = IndexOf(node_fields, "name");
  node_address_offset = IndexOf(node_fields, "id");
  node_self_size_offset = IndexOf(node_fields, "self_size");
  node_edge_count_offset = IndexOf(node_fields, "edge_count");
  node_trace_nodeid_offset = IndexOf(node_fields, "trace_node_id");
  edge_type_offset = IndexOf(edge_fields, "type");
  edge_name_or_index_offset = IndexOf(edge_fields, "name_or_index");
  edge_to_node_offset = IndexOf(edge_fields, "to_node");
  edge_from_node = new long[edge_count];
  first_edge_indexes = GetFirstEdgeIndexes();
  node_util = new snapshot_node::Node(this);
  edge_util = new snapshot_edge::Edge(this);
}

int SnapshotParser::IndexOf(json array, std::string target) {
  const char* t = target.c_str();
  int size = array.size();
  for (int i = 0; i < size; i++) {
    std::string str1 = array[i];
    if(strcmp(str1.c_str(), t) == 0) {
      return i;
    }
  }
  return -1;
}

long* SnapshotParser::GetFirstEdgeIndexes() {
  long* first_edge_indexes = new long[node_count];
  for(long node_ordinal = 0, edge_index = 0; node_ordinal < node_count; node_ordinal++) {
    first_edge_indexes[node_ordinal] = edge_index;
    int offset = static_cast<int>(nodes[node_ordinal * node_field_length + node_edge_count_offset]) * edge_field_length;
    for(int i = edge_index; i < offset; i += edge_field_length) {
      edge_from_node[i / edge_field_length] = node_ordinal;
    }
    edge_index += offset;
  }
  return first_edge_indexes;
}

void SnapshotParser::CreateAddressMap() {
  for(long ordinal = 0; ordinal < node_count; ordinal++) {
    long address = node_util->GetAddress(ordinal, false);
    address_map_.insert(AddressMap::value_type(address, ordinal));
  }
}

void SnapshotParser::ClearAddressMap() {
  address_map_.clear();
}

long SnapshotParser::SearchOrdinalByAddress(long address) {
  int count = address_map_.count(address);
  if(count == 0) {
    return -1;
  }
  long ordinal = address_map_.at(address);
  return ordinal;
}

void SnapshotParser::BuildTotalRetainer() {
  retaining_nodes_ = new long[edge_count] {0};
  retaining_edges_ = new long[edge_count] {0};
  first_retainer_index_ = new long[node_count + 1] {0};
  // every node's retainer count
  for(long to_node_field_index = edge_to_node_offset, l = static_cast<long>(edges.size()); to_node_field_index < l; to_node_field_index += edge_field_length) {
    long to_node_index = static_cast<long>(edges[to_node_field_index]);
    if(to_node_index % node_field_length != 0) {
      Nan::ThrowTypeError(Nan::New<v8::String>("node index id is wrong!").ToLocalChecked());
      return;
    }
    long ordinal_id = to_node_index / node_field_length;
    first_retainer_index_[ordinal_id] += 1;
  }
  // set first retainer index
  for(long i = 0, first_unused_retainer_slot = 0; i < node_count; i++) {
    long retainers_count = first_retainer_index_[i];
    first_retainer_index_[i] = first_unused_retainer_slot;
    retaining_nodes_[first_unused_retainer_slot] = retainers_count;
    first_unused_retainer_slot += retainers_count;
  }
  // for (index ~ index + 1)
  first_retainer_index_[node_count] = edge_count;
  // set retaining slot
  long next_node_first_edge_index = first_edge_indexes[0];
  for(long src_node_ordinal = 0; src_node_ordinal < node_count; src_node_ordinal++) {
    long first_edge_index = next_node_first_edge_index;
    next_node_first_edge_index = first_edge_indexes[src_node_ordinal + 1];
    for(long edge_index = first_edge_index; edge_index < next_node_first_edge_index; edge_index += edge_field_length) {
      long to_node_index = static_cast<long>(edges[edge_index + edge_to_node_offset]);
      if(to_node_index % node_field_length != 0) {
        Nan::ThrowTypeError(Nan::New<v8::String>("to_node id is wrong!").ToLocalChecked());
        return;
      }
      long first_retainer_slot_index = first_retainer_index_[to_node_index / node_field_length];
      long next_unused_retainer_slot_index = first_retainer_slot_index + (--retaining_nodes_[first_retainer_slot_index]);
      // save retainer & edge
      retaining_nodes_[next_unused_retainer_slot_index] = src_node_ordinal;
      retaining_edges_[next_unused_retainer_slot_index] = edge_index;
    }
  }
}

int SnapshotParser::GetRetainersCount(long id) {
  long first_retainer_index = first_retainer_index_[id];
  long next_retainer_index = first_retainer_index_[id + 1];
  // count may not be larger than 2^31
  return static_cast<int>(next_retainer_index - first_retainer_index);
}

long* SnapshotParser::GetRetainers(long id) {
  long first_retainer_index = first_retainer_index_[id];
  long next_retainer_index = first_retainer_index_[id + 1];
  int length = static_cast<int>(next_retainer_index - first_retainer_index);
  long* retainers = new long[length * 2];
  for(long i = first_retainer_index; i < next_retainer_index; i++) {
    long node = retaining_nodes_[i];
    long edge = retaining_edges_[i];
    retainers[(i - first_retainer_index) * 2] = node;
    retainers[(i - first_retainer_index) * 2 + 1] = edge;
  }
  return retainers;
}

void SnapshotParser::EnqueueNode_(snapshot_distance_t* t) {
  if(t->node_distances_[t->ordinal] != NO_DISTANCE)
    return;
  t->node_distances_[t->ordinal] = t->distance;
  t->node_to_visit[*(t->node_to_visit_length)] = t->ordinal;
  *(t->node_to_visit_length) += 1;
}

bool SnapshotParser::Filter(long ordinal, long edge) {
  int node_type = node_util->GetTypeForInt(ordinal, false);
  if(node_type == snapshot_node::NodeTypes::KHIDDEN) {
    std::string edge_name = edge_util->GetNameOrIndex(edge, true);
    std::string node_name = node_util->GetName(ordinal, false);
    std::string slow_function_map_name = "sloppy_function_map";
    std::string native_context = "system / NativeContext";
    return (strcmp(edge_name.c_str(), slow_function_map_name.c_str()) != 0) || (strcmp(node_name.c_str(), native_context.c_str()) != 0);
  }
  if(node_type == snapshot_node::NodeTypes::KARRAY) {
    std::string node_name = node_util->GetName(ordinal, false);
    std::string map_descriptors_name = "(map descriptors)";
    if(strcmp(node_name.c_str(), map_descriptors_name.c_str()) != 0)
      return true;
    std::string edge_name = edge_util->GetNameOrIndex(edge, true);
    int index = atoi(edge_name.c_str());
    return index < 2 || (index % 3) != 1;
  }
  return true;
}

void SnapshotParser::ForEachRoot_(void (*action)(snapshot_distance_t* t), snapshot_distance_t* user_root, bool user_root_only) {
  std::unordered_map<long, bool> visit_nodes;
  long gc_roots = -1;
  long* edges = node_util->GetEdges(root_index, false);
  int length = node_util->GetEdgeCount(root_index, false);
  for(int i = 0; i < length; i++) {
    long target_node = edge_util->GetTargetNode(*(edges + i), true);
    std::string node_name = node_util->GetName(target_node, false);
    std::string gc_root_name = "(GC roots)";
    if(strcmp(node_name.c_str(), gc_root_name.c_str()) == 0) {
      gc_roots = target_node;
    }
  }
  if(gc_roots == -1)
    return;
  if(user_root_only) {
    // iterator the "true" root, set user root distance 1 -> global
    for(int i = 0; i < length; i++) {
      long target_node = edge_util->GetTargetNode(*(edges + i), true);
      std::string node_name = node_util->GetName(target_node, false);
      int type = node_util->GetTypeForInt(target_node, false);
      // type != synthetic, means user root
      if(type != snapshot_node::NodeTypes::KSYNTHETIC) {
        if(visit_nodes.count(target_node) == 0) {
          user_root->ordinal = target_node;
          action(user_root);
          visit_nodes.insert(std::unordered_map<long, bool>::value_type(target_node, true));
        }
      }
    }
    // set user root gc roots -> synthetic roots -> true roots
    // long* sub_root_edges = node_util->GetEdges(gc_roots, false);
    // int sub_root_edge_length = node_util->GetEdgeCount(gc_roots, false);
    // for(int i = 0; i < sub_root_edge_length; i++) {
    //   long sub_root_ordinal = edge_util->GetTargetNode(*(sub_root_edges + i), true);
    //   long* sub2_root_edges = node_util->GetEdges(sub_root_ordinal, false);
    //   int sub2_root_edge_length = node_util->GetEdgeCount(sub_root_ordinal, false);
    //   for(int j = 0; j < sub2_root_edge_length; j++) {
    //     long sub2_root_ordinal = edge_util->GetTargetNode(*(sub2_root_edges + j), true);
    //     // mark sub sub gc roots
    //     if(visit_nodes.count(sub2_root_ordinal) == 0) {
    //       user_root->ordinal = sub2_root_ordinal;
    //       action(user_root);
    //       visit_nodes.insert(std::unordered_map<long, bool>::value_type(sub2_root_ordinal, true));
    //     }
    //   }
    // }
  } else {
    long* sub_root_edges = node_util->GetEdges(gc_roots, false);
    int sub_root_edge_length = node_util->GetEdgeCount(gc_roots, false);
    for(int i = 0; i < sub_root_edge_length; i++) {
      long sub_root_ordinal = edge_util->GetTargetNode(*(sub_root_edges + i), true);
      long* sub2_root_edges = node_util->GetEdges(sub_root_ordinal, false);
      int sub2_root_edge_length = node_util->GetEdgeCount(sub_root_ordinal, false);
      for(int j = 0; j < sub2_root_edge_length; j++) {
        long sub2_root_ordinal = edge_util->GetTargetNode(*(sub2_root_edges + j), true);
        // mark sub sub gc roots
        if(visit_nodes.count(sub2_root_ordinal) == 0) {
          user_root->ordinal = sub2_root_ordinal;
          action(user_root);
          visit_nodes.insert(std::unordered_map<long, bool>::value_type(sub2_root_ordinal, true));
        }
      }
      // mark sub gc roots
      if(visit_nodes.count(sub_root_ordinal) == 0) {
        user_root->ordinal = sub_root_ordinal;
        action(user_root);
        visit_nodes.insert(std::unordered_map<long, bool>::value_type(sub_root_ordinal, true));
      }
    }
    // mark sub roots
    for(int i = 0; i < length; i++) {
      long target_node = edge_util->GetTargetNode(*(edges + i), true);
      if(visit_nodes.count(target_node) == 0) {
        user_root->ordinal = target_node;
        action(user_root);
        visit_nodes.insert(std::unordered_map<long, bool>::value_type(target_node, true));
      }
    }
  }
}

void SnapshotParser::BFS_(long* node_to_visit, int node_to_visit_length) {
  int index = 0;
  int temp = 0;
  while(index < node_to_visit_length) {
    long ordinal = node_to_visit[index++];
    int distance = node_distances_[ordinal] + 1;
    long* edges = node_util->GetEdges(ordinal, false);
    int edge_length = node_util->GetEdgeCount(ordinal, false);
    for(int i = 0; i < edge_length; i++) {
      int edge_type = edge_util->GetTypeForInt(*(edges + i), true);
      // ignore weak edge
      if(edge_type == snapshot_edge::EdgeTypes::KWEAK)
        continue;
      int child_ordinal = edge_util->GetTargetNode(*(edges + i), true);
      if(node_distances_[child_ordinal] != NO_DISTANCE)
        continue;
      // need optimized filter
      // if(!Filter(ordinal, *(edges + i)))
      // continue;
      node_distances_[child_ordinal] = distance;
      node_to_visit[node_to_visit_length++] = child_ordinal;
      temp++;
    }
  }
  if (node_to_visit_length > node_count) {
    std::string error = "BFS failed. Nodes to visit (" + std::to_string(node_to_visit_length)
                        + ") is more than nodes count (" + std::to_string(node_count) + ")";
    Nan::ThrowTypeError(Nan::New<v8::String>(error).ToLocalChecked());
  }
}

void SnapshotParser::BuildDistances() {
  node_distances_ = new int[node_count];
  for(long i = 0; i < node_count; i++) {
    *(node_distances_ + i) = NO_DISTANCE;
  }
  long* node_to_visit = new long[node_count] {0};
  int node_to_visit_length = 0;
  // add user root
  snapshot_distance_t* user_root = new snapshot_distance_t;
  user_root->distance = 1;
  user_root->node_to_visit = node_to_visit;
  user_root->node_to_visit_length = &node_to_visit_length;
  user_root->node_distances_ = node_distances_;
  ForEachRoot_(EnqueueNode_, user_root, true);
  BFS_(node_to_visit, node_to_visit_length);
  // add rest
  node_to_visit_length = 0;
  user_root->distance = BASE_SYSTEMDISTANCE;
  ForEachRoot_(EnqueueNode_, user_root, false);
  BFS_(node_to_visit, node_to_visit_length);
  user_root = nullptr;
}

int SnapshotParser::GetDistance(long id) {
  return node_distances_[id];
}
}