#include <iostream>

#include "profile_util.h"
#include <BasicProjectionTargets.h>
#include <DMesh3.h>
#include <DMeshAABBTree3.h>
#include <MeshQueries.h>
#include <MeshSubdivider.h>
#include <OBJReader.h>
#include <OBJWriter.h>
#include <Remesher.h>
#include <VectorUtil.h>
#include <refcount_vector.h>
#include <small_list_set.h>
#include "MeshboundaryLoop.h"

using namespace g3;

void PreserveAllBoundaryEdges(MeshConstraintsPtr cons, DMesh3Builder::PDMesh3 p_mesh) {
	if (!p_mesh) {
		return;
	}
	if (p_mesh->EdgeCount() <= 1) {
		return;
	}
	int32_t max_edge_id = p_mesh->MaxEdgeID();
	for (int edge_id = 0; edge_id < max_edge_id; ++edge_id) {
		if (p_mesh->IsEdge(edge_id) && p_mesh->IsBoundaryEdge(edge_id)) {
			cons->SetOrUpdateEdgeConstraint(edge_id,
				EdgeConstraint::FullyConstrained());

			Index2i ev = p_mesh->GetEdgeV(edge_id);
			VertexConstraint vc = VertexConstraint::Pinned();
			cons->SetOrUpdateVertexConstraint(ev.x(), vc);
			cons->SetOrUpdateVertexConstraint(ev.y(), vc);
		}
	}
}

// https://github.com/gradientspace/geometry3Sharp/blob/master/mesh/MeshConstraintUtil.cs
/// <summary>
/// Remove 'fin' triangles that have only one connected triangle.
/// Removing one fin can create another, by default will keep iterating
/// until all fins removed (in a not very efficient way!).
/// Pass bRepeatToConvergence=false to only do one pass.
/// [TODO] if we are repeating, construct face selection from numbers of first
// list and iterate over that on future passes!
/// </summary>
size_t RemoveFinTriangles(DMesh3Builder::PDMesh3 mesh, bool bRepeatToConvergence = true) {
	size_t nRemoved = 0;
	std::list<int> to_remove;
	while (true) {
		for (int tid : mesh->TriangleIndices()) {
			Index3i nbrs = mesh->GetTriNeighbourTris(tid);
			int c = ((nbrs.x() != DMesh3::InvalidID) ? 1 : 0) +
				((nbrs.y() != DMesh3::InvalidID) ? 1 : 0) +
				((nbrs.z() != DMesh3::InvalidID) ? 1 : 0);
			if (c <= 1) {
				to_remove.push_back(tid);
			}
		}
		if (!to_remove.size()) {
			return nRemoved;
		}
		nRemoved += to_remove.size();

		for (int tid : to_remove) {
			mesh->RemoveTriangle(tid, false, true);
		}
		to_remove.clear();
		if (!bRepeatToConvergence) {
			break;
		}
	}
	return nRemoved;
}

//DCurve3Ptr ExtractLoopV(DMesh3Builder::PDMesh3 mesh, std::vector<int> vertices) {
//	DCurve3Ptr curve = std::make_shared<DCurve3>();
//	for (int vid : vertices) {
//		curve->AppendVertex(mesh->GetVertex(vid));
//	}
//	curve->SetClosed(true);
//	return curve;
//}

// https://github.com/gradientspace/geometry3Sharp/blob/master/mesh/MeshConstraintUtil.cs
void PreserveBoundaryLoops(MeshConstraintsPtr cons, DMesh3Builder::PDMesh3 mesh) {
	// MeshBoundaryLoops loops = new MeshBoundaryLoops(mesh);
	//for (int32_t loop_i = 0;;) {
	//	DCurve3 loopC = MeshUtil.ExtractLoopV(mesh, loop.Vertices);
	//	DCurveProjectionTarget target = new DCurveProjectionTarget(loopC);
	//	ConstrainVtxLoopTo(cons, mesh, loop.Vertices, target);
	//}
}

int main(int argc, char** argv) {
	OBJReader reader;
	DMesh3Builder builder;

	std::ifstream input("sailor.obj");

	BlockTimer read_timer("read", true);
	ReadOptions options = ReadOptions::Defaults();
	options.ReadMaterials = true;
	reader.Read(input, options, builder);
	read_timer.Stop();
	std::cout << "read " << builder.Meshes.size() << " meshes, took "
		<< read_timer.ToString() << std::endl;
	if (!builder.Meshes.size()) {
		return 1;
	}
	DMesh3Builder::PDMesh3 mesh1 = builder.Meshes[0];
	std::cout << mesh1->MeshInfoString();
	// TODO 2021-01-21 Color, UV, Groups aren't input // fire

	DMeshAABBTree3 spatialTest(mesh1, true);
	spatialTest.Build();
	spatialTest.TestCoverage();
	BlockTimer remesh_timer("remesh", true);
	Remesher r(mesh1);
	MeshConstraintsPtr cons = std::make_shared<MeshConstraints>();
	PreserveAllBoundaryEdges(cons, mesh1);
	// PreserveBoundaryLoops
	// https://github.com/gradientspace/geometry3Sharp/blob/master/mesh/MeshConstraintUtil.cs
	// preserve group-region-border-loops
	// int set_id = 1;
	// int[][] group_tri_sets = FaceGroupUtil.FindTriangleSetsByGroup(mesh);
	// foreach (int[] tri_list in group_tri_sets) {
	//     MeshRegionBoundaryLoops loops = new MeshRegionBoundaryLoops(mesh,
	//     tri_list); foreach (EdgeLoop loop in loops) {
	//         MeshConstraintUtil.ConstrainVtxLoopTo(r, loop.Vertices,
	//             new DCurveProjectionTarget(loop.ToCurve()), set_id++);
	//     }
	//  }
	r.SetExternalConstraints(cons);
	r.SetProjectionTarget(MeshProjectionTarget::AutoPtr(mesh1, true));
	// http://www.gradientspace.com/tutorials/2018/7/5/remeshing-and-constraints
	int iterations = 5;
	r.SmoothSpeedT /= iterations;
	r.EnableParallelSmooth = true;
	r.PreventNormalFlips = true;
	double avg_edge_len = 0.0;
	for (int32_t edge_i = 1; edge_i < mesh1->EdgeCount(); edge_i++) {
		double edge_len = (mesh1->GetEdgePoint(edge_i - 1, edge_i - 1) -
			mesh1->GetEdgePoint(edge_i - 1, edge_i))
			.norm();
		avg_edge_len += edge_len;
		avg_edge_len /= 2.0;
	}
	std::cout << "avg edge len " << avg_edge_len << std::endl;
	double target_edge_len = avg_edge_len;
	target_edge_len = Clamp(target_edge_len, 0.008, 1.0); // Meters
	std::cout << "target edge len " << target_edge_len << std::endl;
	r.SetTargetEdgeLength(target_edge_len);
	r.Precompute();
	for (int k = 0; k < iterations; ++k) {
		r.BasicRemeshPass();
		std::cout << "remesh pass " << k << std::endl;
	}
	RemoveFinTriangles(mesh1, true);
	remesh_timer.Stop();
	std::cout << "remesh took " << remesh_timer.ToString() << std::endl;
	std::cout << mesh1->MeshInfoString();

	std::ofstream output("output_sailor.obj");
	std::vector<WriteMesh> write_meshes;
	write_meshes.push_back(WriteMesh(mesh1));
	OBJWriter writer;
	writer.Write(output, write_meshes, WriteOptions::Defaults());
}
