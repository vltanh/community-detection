import csv
import sys
import time
import logging
import argparse
from pathlib import Path

import pandas as pd
import networkit as nk


def main(args):
    global quiet

    edgelist = args.edgelist
    output_dir = Path(args.output_directory)
    k = args.kvalue
    quiet = args.quiet

    # ===========

    output_dir.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        filename=output_dir / "run.log",
        filemode="w",
        level=logging.INFO,
        format="%(asctime)s - %(levelname)s - %(message)s",
    )
    logging.getLogger().addHandler(logging.StreamHandler(sys.stdout))

    # ===========

    start = time.perf_counter()

    # Reading the edgelist using Pandas
    logging.info(f"Reading edgelist from {edgelist}...")
    df = pd.read_csv(edgelist)

    # Map string IDs to integer IDs
    unique_nodes = pd.unique(df[["source", "target"]].values.ravel("K"))
    node_map = {name: i for i, name in enumerate(unique_nodes)}
    inverted_node_id_map = {i: name for name, i in node_map.items()}

    # Create Networkit graph
    graph1 = nk.Graph(n=len(unique_nodes), weighted=False, directed=True)
    for row in df.itertuples(index=False):
        graph1.addEdge(node_map[row.source], node_map[row.target])

    # Format graph (removes self loops, handles weights if necessary)
    graph, node_id_dict = format_graph(graph1)

    elapsed = time.perf_counter() - start
    logging.info(f"[TIME] Loading network: {elapsed}")

    # ===========

    start = time.perf_counter()

    clusters = iterative_k_core_decomposition_MCS_ES(graph, k, node_id_dict)

    elapsed = time.perf_counter() - start
    logging.info(f"[TIME] Running IKC algorithm: {elapsed}")

    # ===========

    start = time.perf_counter()

    print_clusters(clusters, output_dir, inverted_node_id_map)

    elapsed = time.perf_counter() - start
    logging.info(f"[TIME] Saving results: {elapsed}")


def print_clusters(clusters, out_dir, inverted_node_id_map):
    """
    Writes the clustering result to com.csv.
    Format: node_id,cluster_id
    - Filters out singleton clusters.
    - Ensures cluster_id starts at 0 and is consecutive.
    """
    output_file = out_dir / "com.csv"

    # Counter for consecutive cluster IDs (0, 1, 2...)
    cluster_id_counter = 0
    singleton_count = 0

    with open(output_file, "w") as output:
        # Change delimiter to comma and add header
        csvwriter = csv.writer(output, delimiter=",", lineterminator="\n")
        csvwriter.writerow(["node_id", "cluster_id"])

        for cluster_info in clusters:
            (cluster, k, modularity_score) = cluster_info

            # Filter singletons
            if len(cluster) <= 1:
                singleton_count += 1
                continue

            # Write all nodes in this cluster
            for node in cluster:
                csvwriter.writerow([inverted_node_id_map[node], cluster_id_counter])

            # Increment cluster ID only after writing a valid cluster
            cluster_id_counter += 1

    if not quiet:
        logging.info(f"Removed {singleton_count} singleton clusters.")
        logging.info(f"Saved {cluster_id_counter} valid communities to {output_file}")


def iterative_k_core_decomposition_MCS_ES(graph, k, inverted_orig_node_ids):
    """
    INPUT
    -----
    graph                  : the full networkit graph
    k                      : the minimum allowed value for k for valid clusters
    inverted_orig_node_ids : the dictionary mapping the compacted node IDs to the original node IDs
    OUTPUT
    ------
    final_clusters : the clustering output, a list of lists with clustered nodes
    """
    orig_graph = nk.graphtools.subgraphFromNodes(graph, graph.iterNodes())
    L = orig_graph.numberOfEdges()
    singletons = []
    final_clusters = []

    nbr_failed_modularity = 0
    nbr_failed_k_valid = 0

    # continue finding clusters for different values of k until
    # a. there are no nodes left in the garph or
    # b. the maximum value of k is lower than the minumum allowed k for valid clusters
    while graph.numberOfNodes() > 0:

        # run kc on the graph to get the smallest kcore
        subgraph, max_k, kcore = kc(graph)
        if subgraph == None:
            if not quiet:
                print("no available subgraph")
            break

        # if b. above is true, add all singletons and nodes left in the graph as individual clusters
        # and break
        if max_k < k:
            for node in graph.iterNodes():
                modularity = (-1) * (
                    orig_graph.degree(inverted_orig_node_ids[node]) / (2 * L)
                ) ** 2
                final_clusters.append(([inverted_orig_node_ids[node]], 0, modularity))
            for node in singletons:
                final_clusters.append(([node], 0, 0))
            break

        # compute the components
        cc = nk.components.WeaklyConnectedComponents(subgraph)
        cc.run()
        components = cc.getComponents()

        nodes_to_remove = set()

        # check components to make sure they are k-valid and m-valid
        # then if so, add them to a cluster or break them up to make k-valid
        # finally add them to the final_clusters and remove those nodes from the graph
        for component in components:
            clusters = []
            valid_modularities = []

            # ensure the component is k_valid and modular
            # if not remode the nodes from the graph and add to the singletons
            if k_valid(component, subgraph, k):
                modularity = modular(component, orig_graph, inverted_orig_node_ids)
                if modularity > 0:
                    sub_components = [(component, modularity)]
                else:
                    if not quiet:
                        print("failed modularity")
                    nbr_failed_modularity += 1
                    sub_components = []
                    nodes_to_remove.update(component)
                    singletons.extend(
                        orig_id_component(component, inverted_orig_node_ids)
                    )
            else:
                if not quiet:
                    print("failed k-valid")
                nbr_failed_k_valid += 1
                sub_components = []
                nodes_to_remove.update(component)
                singletons.extend(orig_id_component(component, inverted_orig_node_ids))

            # retreive the original node ID for the clustering output and add each new component as its own cluster
            for sub_component, modularity in sub_components:
                nodes_to_remove.update(sub_component)
                cluster = []
                for node in sub_component:
                    cluster.append(inverted_orig_node_ids[node])
                if not quiet:
                    print("adding cluster length", len(cluster))
                clusters.append((cluster, max_k, modularity))

            final_clusters.extend(clusters)

        # just prints information about the number of components to standard output
        component_sizes = cc.getComponentSizes()
        nbr_large_components = 0
        large_components = dict()
        for nbr, size in component_sizes.items():
            if size > 100:
                nbr_large_components += 1
                large_components[nbr] = size
        if not quiet:
            print(
                "nbr components:",
                len(components),
                ",  nbr components with more than 100 nodes:",
                nbr_large_components,
            )

        # remove nodes marked for removal
        # (either already clustered or when a large cluster is not validly broken up)
        for node in nodes_to_remove:
            graph.removeNode(node)
            # print ("removing node: ", inverted_orig_node_ids[node])

        # compact the subgraph with continuous node IDs (needed for partitioning in kc)
        # and update inverted_orig_node_ids with the new IDs
        node_id_dict = nk.graphtools.getContinuousNodeIds(graph)
        inverted_node_id_dict = dict(map(reversed, node_id_dict.items()))
        temp_dict = dict()
        for new_id, old_id in inverted_node_id_dict.items():
            orig_id = inverted_orig_node_ids[old_id]
            temp_dict[new_id] = orig_id
        inverted_orig_node_ids = temp_dict
        graph = nk.graphtools.getCompactedGraph(graph, node_id_dict)

        if not quiet:
            print("nodes left in graph: ", graph.numberOfNodes())

    if not quiet:
        print(
            "nbr of clusters which were rejected since they were not k-valid : ",
            nbr_failed_k_valid,
        )
        print(
            "nbr of clusters which were rejected since they were not modular : ",
            nbr_failed_modularity,
        )

    return final_clusters


def kc(graph, k=None):
    """
    INPUT
    -----
    graph : networkit graph
    k     : the minimum node degree for all nodes in the subgraph
    OUTPUT
    ------
    subgraph : the subgraph containing only nodes of degree k or higher
    max_k    : the the largest value of k for which there are still nodes remaining in the subgraph
    kc       : the core decomposition networkit object
    """

    # get the value of k associated with each node in the graph and store in a partition
    kc = nk.centrality.CoreDecomposition(graph, storeNodeOrder=True)
    kc.run()
    partition = kc.getPartition()

    kcore_members = []
    max_k = kc.maxCoreNumber()

    # default to the maximum k value
    if k == None:
        k = max_k

    save_k = k

    # if the value of k is lower than the minimum allowed exit
    if max_k < k:
        return None, max_k, kc

    # populate the kcore members with all nodes with k values between k and max_k
    while k <= max_k:
        kcore_members.extend(partition.getMembers(k))
        k += 1

    # return the subgraph with nodes from kcore members
    if not quiet:
        print("k value", save_k, "nbr core members", len(kcore_members))
    return nk.graphtools.subgraphFromNodes(graph, kcore_members), max_k, kc


def k_valid(component, subgraph, k):
    # subgraph = nk.graphtools.subgraphFromNodes(graph, component)
    k_valid = True
    component_nodes = set(component)
    for node in subgraph.iterNodes():
        if node in component_nodes:
            if (subgraph.degreeIn(node) + subgraph.degreeOut(node)) < k:
                # print ("node", node)
                # print('fails k condition', subgraph.degree(node))
                k_valid = False
                break
    return k_valid


def modular(component, orig_graph, inverted_orig_node_ids):
    POSITIVE_VALUE = 1
    return POSITIVE_VALUE

    cluster = nk.graphtools.subgraphFromNodes(
        orig_graph, orig_id_component(component, inverted_orig_node_ids)
    )

    l = orig_graph.numberOfEdges()
    ls = cluster.numberOfEdges()
    ds = 0

    for node in cluster.iterNodes():
        ds += orig_graph.degreeIn(node)
        ds += orig_graph.degreeOut(node)

    return ls / l - (ds / (2 * l)) ** 2


def orig_id_component(component, inverted_orig_node_ids):
    return [inverted_orig_node_ids[node] for node in component]


def format_graph(graph1):
    origNodeIdDict = nk.graphtools.getContinuousNodeIds(graph1)
    invertedOrigNodeIdDict = dict(map(reversed, origNodeIdDict.items()))
    graph1 = nk.graphtools.getCompactedGraph(graph1, origNodeIdDict)

    if graph1.isWeighted() == False:
        if not quiet:
            print("not weighted")
        weighted = nk.Graph(n=0, weighted=True, directed=True)

        for node in graph1.iterNodes():
            weighted.addNode()
        for u, v in graph1.iterEdges():
            if weighted.hasNode(u) == False:
                weighted.addNode(u)
            if weighted.hasNode(v) == False:
                weighted.addNode(v)
            weighted.addEdge(u, v, graph1.degreeIn(u))
            # print (u, v, graph1.degreeIn(u))
        graph = weighted
    else:
        graph = graph1

    graph.removeSelfLoops()

    if not quiet:
        print(graph.numberOfNodes())
    return graph, invertedOrigNodeIdDict


def parseArgs():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "-e",
        "--edgelist",
        type=str,
        help="Path to file containing edge lists",
        required=True,
        default=None,
    )

    parser.add_argument(
        "-o",
        "--output-directory",
        type=str,
        help="Path to file containing output",
        required=True,
        default=None,
    )

    parser.add_argument(
        "-k",
        "--kvalue",
        type=int,
        help="non-negative integer value of the minimum required adjacent nodes for each node",
        required=False,
        default=0,
    )

    parser.add_argument(
        "-q", "--quiet", action="store_true", help="silence ikc outputs"
    )

    parser.add_argument(
        "-v",
        "--version",
        action="version",
        version="1.0.0",
        help="show the version number and exit",
    )

    return parser.parse_args()


if __name__ == "__main__":
    main(parseArgs())
