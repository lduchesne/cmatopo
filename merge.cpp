#include <merge.h>

#include <zones.h>

#include <chrono>
#include <memory>
#include <vector>
#include <functional>
#include <boost/mpi/collectives.hpp>
#include <boost/mpi/communicator.hpp>

using namespace std;
using namespace boost::mpi;

namespace cma {

typedef vector<int> itemid_map;

void merge_topologies(Topology& t1, Topology& t2)
{
    assert (t1._transactions->empty());
    assert (t2._transactions->empty());

    unique_ptr<itemid_map> node_map(new itemid_map(t2._nodes.size(), -1));
    unique_ptr<itemid_map> edge_map(new itemid_map(t2._edges.size(), -1));
    unique_ptr<itemid_map> face_map(new itemid_map(t2._faces.size(), -1));
    unique_ptr<itemid_map> relation_map(new itemid_map(t2._relations.size(), -1));

    // universal face stays the same even after merge
    (*face_map)[0] = 0;

    int nextNodeId;
    int newEdgeId, nextEdgeId;
    int nextFaceId;
    int nextTopogeoId;

    nextNodeId = t1._nodes.size();
    newEdgeId = nextEdgeId = t1._edges.size();
    nextFaceId = t1._faces.size();
    nextTopogeoId = t1._relations.size();

    for (int nodeId = 1; nodeId < t2._nodes.size(); ++nodeId) {
        node* n = t2._nodes[nodeId];
        if (n) {
            (*node_map)[n->id] = nextNodeId;
            n->id = nextNodeId;
        }
        t1._nodes.push_back(n);
        ++nextNodeId;
    }

    for (int edgeId = 1; edgeId < t2._edges.size(); ++edgeId) {
        edge* e = t2._edges[edgeId];
        if (e) {
            (*edge_map)[e->id] = nextEdgeId;
            e->id = nextEdgeId;
        }
        t1._edges.push_back(e);
        ++nextEdgeId;
    }

    for (face* f : t2._faces) {
        if (f && f->id == 0) continue;

        if (f) {
            (*face_map)[f->id] = nextFaceId;
            f->id = nextFaceId;
        }
        t1._faces.push_back(f);
        ++nextFaceId;
    }

    for (int topogeoId = 1; topogeoId < t2._relations.size(); ++topogeoId) {
        (*relation_map)[topogeoId] = nextTopogeoId;

        vector<relation*>* relations = t2._relations[topogeoId];
        if (relations) {
            for (relation* r : *relations) {
                r->topogeo_id = nextTopogeoId;
                switch (r->element_type)
                {
                case 2:     // LINESTRING (edge)
                    r->element_id = (*edge_map)[r->element_id];
                    break;
                case 3:     // FACE
                    r->element_id = (*face_map)[r->element_id];
                    break;
                default:
                    assert (false);
                }
            }
        }

        t1._relations.push_back(relations);
        ++nextTopogeoId;
    }

    for (auto& p : *t2._topogeom_relations) {
        p.second = (*relation_map)[p.second];
    }
    t1._topogeom_relations->insert(
        t2._topogeom_relations->begin(), t2._topogeom_relations->end());

    for (int i = newEdgeId; i < t1._edges.size(); ++i) {
        edge* e = t1._edges[i];
        if (!e) continue;

        e->start_node = (*node_map)[e->start_node];
        e->end_node   = (*node_map)[e->end_node];

        e->next_left_edge  = e->next_left_edge  < 0 ? -(*edge_map)[abs(e->next_left_edge)]  : (*edge_map)[e->next_left_edge];
        e->next_right_edge = e->next_right_edge < 0 ? -(*edge_map)[abs(e->next_right_edge)] : (*edge_map)[e->next_right_edge];

        e->abs_next_left_edge  = (*edge_map)[e->abs_next_left_edge];
        e->abs_next_right_edge = (*edge_map)[e->abs_next_right_edge];

        e->left_face  = (*face_map)[e->left_face];
        e->right_face = (*face_map)[e->right_face];
    }

    t2._empty(false);
}

int merge_topologies(
    PG& db,
    GEOSHelper* geos,
    vector<zone*> zones,
    vector<int>& topologies,
    vector<zone*>& new_zones,
    bool merge_restore)
{
    assert (new_zones.empty());

    communicator world;

    int orphan_count = 0;

    assert (topologies.size() % 4 == 0);

    for (int i = 0; i < topologies.size()/4; ++i)
    {
        vector<zone*> temp_new_zones;
        Topology* t[2] = {nullptr, nullptr};

        for (int j = 0; j < 2; ++j)
        {
            Topology* t1 = restore_topology(geos, get_zone_by_id(zones, topologies[i*4+j*2]), false);
            Topology* t2 = restore_topology(geos, get_zone_by_id(zones, topologies[i*4+j*2+1]), false);
            if (!t1) {
                cout << "[" << world.rank() << "] (fatal t1) topology for zone #" << topologies[i*4+j*2] << " could not be restored" << endl;
            }
            if (!t2) {
                cout << "[" << world.rank() << "] (fatal t2) topology for zone #" << topologies[i*4+j*2+1] << " could not be restored" << endl;
            }
            assert (t1 && t2);

            int z2_id = t2->zoneId();

            Topology* t1t = t1;
            orphan_count +=
                _internal_merge(db, geos, zones, &t1, t2, temp_new_zones, merge_restore);
            if (t1 != t1t) {
                // a swap occured
                topologies[i*4+j*2] = t1->zoneId();
            }

            t[j] = t1;

            zones.erase(find_if(zones.begin(), zones.end(), [t1](const zone* z) {
                return z->id() == t1->zoneId();
            }));
            zones.erase(find_if(zones.begin(), zones.end(), [z2_id](const zone* z) {
                return z->id() == z2_id;
            }));
        }
        assert (t[0] && t[1]);
        assert (temp_new_zones.size() == 2);

        // replace zones in the original vector with the new (temporary) ones
        zones.insert(zones.end(), temp_new_zones.begin(), temp_new_zones.end());

        int z2_id = t[1]->zoneId();

        orphan_count += _internal_merge(db, geos, zones, &t[0], t[1], new_zones, merge_restore);

        zones.erase(find_if(zones.begin(), zones.end(), [t](const zone* z) {
            return z->id() == t[0]->zoneId();
        }));
        zones.erase(find_if(zones.begin(), zones.end(), [z2_id](const zone* z) {
            return z->id() == z2_id;
        }));
        zones.push_back(new_zones[new_zones.size()-1]);

        // delete temporary zones
        for (zone* z : temp_new_zones) {
            delete z;
        }

        // merged topology has already been saved in _internal_merge
        delete t[0];

        int progress = int(float((i+1)) / (topologies.size()/4) * 100.0);
        cout << "[" << world.rank() << "] progress: " << progress << "%" << endl;
    }
    topologies.clear();

    int total_orphan_count;
    reduce(world, orphan_count, total_orphan_count, std::plus<int>(), 0);
    return total_orphan_count;
}

void get_next_groups(
    vector<depth_group_t>& all_groups,
    vector<depth_group_t>& next_groups
)
{
    int current_depth = all_groups[0].first;
    next_groups.insert(
        begin(next_groups),
        all_groups.begin(),
        find_if_not(
            all_groups.begin(),
            all_groups.end(),
            [current_depth](const depth_group_t& g) {
                return g.first == current_depth;
            }
        )
    );

    // TODO: to speed things up, also add zones which can
    // be independently merged at other depths too

    all_groups.erase(
        begin(all_groups),
        find_if_not(
            begin(all_groups),
            end(all_groups),
            [current_depth](const depth_group_t& a) {
                return a.first == current_depth;
            }
        )
    );
}


double width(const OGREnvelope& envelope)
{
    return envelope.MaxX - envelope.MinX;
}

double height(const OGREnvelope& envelope)
{
    return envelope.MaxY - envelope.MinY;
}

direction_type position(const OGREnvelope& e1, const OGREnvelope& e2)
{
    if (e1.MinX == e2.MinX && e1.MaxX == e2.MaxX) {
        if (e1.MaxY == e2.MinY) {
            return ABOVE;
        }
        if (e1.MinY == e2.MaxY) {
            return BELOW;
        }
    }

    if (e1.MinY == e2.MinY && e1.MaxY == e2.MaxY) {
        if (e1.MaxX == e2.MinX) {
            return RIGHT;
        }
        if (e1.MinX == e2.MaxX) {
            return LEFT;
        }
    }

    return OTHER;
}

int _internal_merge(
    PG& db,
    GEOSHelper* geos,
    vector<zone*>& zones,
    Topology** t1,
    Topology* t2,
    vector<zone*>& new_zones,
    bool merge_restore)
{
    communicator world;

    cout << "_internal_merge merge_restore: " << (merge_restore ? "true" : "false") << endl;

    // prepare the merged zone right now to see if we have a checkpoint file
    zone* z1 = get_zone_by_id(zones, (*t1)->zoneId());
    zone* z2 = get_zone_by_id(zones, t2->zoneId());

    OGREnvelope envelope = z1->envelope();
    envelope.Merge(z2->envelope());
    zone* merged_zone = new zone((*t1)->zoneId(), envelope);

    Topology* restored = nullptr;
    if (merge_restore) {
        restored = restore_topology(geos, merged_zone, false);
    }

    if (restored) {
        // swap the restored geometry with the current (unmerged) one
        delete *t1;
        *t1 = restored;
    }
    else {
        cout << "[" << world.rank() << "] will merge topologies " << (*t1)->zoneId()
             << " and " << t2->zoneId() << endl;
        merge_topologies(**t1, *t2);
    }
    delete t2;

    cout << "[" << world.rank() << "] merge done (or restored)" << endl;

    linesV orphans;
    size_t orphan_count;
    if (!restored || (*t1)->orphan_count() == -1) {         // -1 is for version 0 serializations
        db.get_common_lines(z1->envelope(), z2->envelope(), orphans);
        cout << "[" << world.rank() << "] adding " << orphans.size() << " lines to topology #"
             << (*t1)->zoneId() << " (lc: " << z1->count() << "/" << (*t1)->count() << "+" << z2->count() << "/" << t2->count() << ")" << endl;
        orphan_count = orphans.size();
    }
    else {
        orphan_count = (*t1)->orphan_count();
    }

    merged_zone->count(z1->count() + z2->count() + orphan_count);
    new_zones.push_back(merged_zone);

    if (restored) {
        (*t1)->orphan_count() = orphan_count;
        save_topology(geos, merged_zone, *t1);

        for (auto& orphan : orphans) {
            GEOSGeometry *geom = orphan.second;
            GEOSGeom_destroy_r(hdl, geom);
        }
        orphans.clear();

        return orphan_count;
    }

    if (!orphans.empty()) {
        cout << "[" << world.rank() << "] rebuilding index..." << endl;
        auto start = chrono::steady_clock::now();
        (*t1)->rebuild_indexes();
        auto end = chrono::steady_clock::now();
        auto elapsed = chrono::duration_cast<chrono::milliseconds>(end - start);
        cout << "[" << world.rank() << "] took " << elapsed.count() << " ms." << endl;
    }

    auto start = chrono::steady_clock::now();
    int lc = 0;
    for (pair<int, GEOSGeometry*>& orphan : orphans) {
        int lineId = orphan.first;
        GEOSGeometry* line = orphan.second;
        try {
            (*t1)->TopoGeo_AddLineString(lineId, line, DEFAULT_TOLERANCE);
            (*t1)->commit();
        }
        catch (const invalid_argument& ex) {
            (*t1)->rollback();
        }
        GEOSGeom_destroy_r(hdl, line);
        if (++lc % 5 == 0) {
            cout << "[" << world.rank() << "] " << lc << endl;
        }
    }
    auto end = chrono::steady_clock::now();
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(end - start);

    cout << "[" << world.rank() << "] added new merged topology for"
         << " zone #" << (*t1)->zoneId() << " (lc: " << merged_zone->count() << ") -- took: " << elapsed.count() << " ms." << endl;
    (*t1)->print_stats();

    // orphan lines were already deleted in the above loop
    orphans.clear();

    save_topology(geos, merged_zone, *t1);

    return orphan_count;
}

} // namespace cma
