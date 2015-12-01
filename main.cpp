#include <ctime>
#include <chrono>
#include <cassert>
#include <fstream>
#include <iostream>
#include <functional>

#include <geos_c.h>
#include <ogrsf_frmts.h>

#include <pg.h>
#include <merge.h>
#include <utils.h>
#include <zones.h>
#include <topology.h>

#include <boost/mpi/collectives.hpp>
#include <boost/mpi/environment.hpp>
#include <boost/mpi/communicator.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/string.hpp>
#include <boost/algorithm/string/join.hpp>

using namespace cma;
using namespace std;

using namespace boost::mpi;
using namespace boost::algorithm;

namespace cma {
    GEOSContextHandle_t hdl;
}

bool slave_exchange_topologies(
    vector<Topology*>& currentTopologies,
    vector<Topology*>& newTopologies);

void exchange_topologies(
    pair<int, int>& fz1,
    pair<int, int>& fz2,
    vector<Topology*>& currentTopologies,
    vector<Topology*>& newTopologies);

int main(int argc, char **argv)
{
    environment env;
    communicator world;

    initGEOS(geos_message_function, geos_message_function);
    OGRRegisterAll();

    unique_ptr<GEOSHelper> geos(new GEOSHelper());
    assert (hdl != NULL);

    // vm with Quebec only: postgresql://postgres@192.168.56.101/postgis
    // vm with the world: postgresql://pgsql@pg/cmdb
    // PG db("postgresql://postgres@localhost/postgis");
    PG db("postgresql://laurent@localhost/cmatopo");
    if (!db.connected()) {
        cerr << "Could not connect to PostgreSQL." << endl;
        return 1;
    }

    int line_count = db.get_line_count();
    assert (line_count >= 0);

    vector<int>* zonesPerProcess = new vector<int>(world.size());
    vector< list<zone*> >* processZones =
        new vector< list<zone*> >(world.size());

    assert (zonesPerProcess->size() == processZones->size());

    for (int i = 0; i < zonesPerProcess->size(); ++i) {
        (*zonesPerProcess)[i] = 0;
    }

    vector<zone*> zones;
    vector<zone*> orderedZones;
    vector<depth_group_t> groups;
    int processingLineCount = 0;
    if (world.rank() == 0) {
        GEOSGeometry* world_extent = world_geom();
        cout << "world geom: " << geos->as_string(world_extent) << endl;
        int nextZoneId = 0;
        prepare_zones(*geos, world_extent, zones, groups, 20);
        GEOSGeom_destroy_r(hdl, world_extent);

        // order groups by depth (furthest first)
        sort(
            groups.begin(), groups.end(),
            [](const depth_group_t& lhs, const depth_group_t& rhs) {
                return lhs.first < rhs.first;
            }
        );

        // keep an original order copy of generated zones
        orderedZones = zones;
        sort(zones.begin(), zones.end(), [](const zone* a, const zone* b) {
            return a->count() > b->count();
        });

        int zoneId = 0;
        for (zone* z : zones) {
            int minProcess = distance(
                zonesPerProcess->begin(),
                min_element(
                    zonesPerProcess->begin(),
                    zonesPerProcess->end()
                )
            );
            assert (minProcess < zonesPerProcess->size());
            (*zonesPerProcess)[minProcess] += z->count()^3;
            processingLineCount += z->count();

            (*processZones)[minProcess].push_back(z);
        }

        /*
        for (int i = 0; i < zonesPerProcess->size(); ++i) {
            cout << "[0] Process " << i << " will process " << sqrt((*zonesPerProcess)[i])
                 << " lines." << endl;
            processingLineCount += sqrt((*zonesPerProcess)[i]);
        }
        */

        cout << "Will process " << processingLineCount << ", leaving " << line_count-processingLineCount
             << " orphans (" << (line_count-processingLineCount)/float(processingLineCount)*100 << "%)" << endl;
    }

    list<zone*> myZones;
    scatter(world, *processZones, myZones, 0);

    processZones->clear();

    delete processZones;

    vector<Topology*> myTopologies;

    int pline = 0;
    for (zone* z : myZones) {
        int zoneId = z->id();
        const string& hexWKT = geos->as_string(z->geom());

        chrono::time_point<chrono::system_clock> start, end;
        start = chrono::system_clock::now();

        GEOSGeometry* zoneGeom =
            GEOSWKTReader_read_r(hdl, geos->text_reader(), hexWKT.c_str());

        linesV lines;
        if (!db.get_lines(zoneGeom, lines, true)) { // 1000
            assert (false);
        }

        pline += lines.size();

        GEOSGeom_destroy_r(hdl, zoneGeom);

        cout << "[" << world.rank() << "] processing zone #" <<  zoneId
             << " (" << lines.size() << ") lines." << endl;

        if (lines.size() == 0) {
            Topology* topology = new Topology(geos.get());
            topology->zoneId(z->id());
            myTopologies.push_back(topology);
            continue;
        }

        Topology* topology = new Topology(geos.get());
        topology->zoneId(z->id());

        int lc = 0;
        for (GEOSGeometry* line : lines) {
            try {
                topology->TopoGeo_AddLineString(line, DEFAULT_TOLERANCE);
                topology->commit();
            }
            catch (const invalid_argument& ex) {
                cerr << geos->as_string(line) << ": " << ex.what() << endl;
                topology->rollback();
            }

            GEOSGeom_destroy_r(hdl, line);

            if (++lc % 100 == 0) {
                cout << lc << endl;
            }
        }
        lines.clear();

        end = chrono::system_clock::now();
        chrono::duration<double> elapsed_seconds = end-start;
        time_t end_time = chrono::system_clock::to_time_t(end);

        cout << "[" << world.rank() << "] finished computation of zone #" << zoneId
             << " at " << std::ctime(&end_time) << ","
             << " elapsed time: " << elapsed_seconds.count() << "s" << endl;

        myTopologies.push_back(topology);
    }

    broadcast(world, zones, 0);

    int allplines;
    reduce(world, pline, allplines, std::plus<int>(), 0);

    if (world.rank() == 0) {
        cout << "processed " << allplines << " lines in first pass." << endl;
        //assert (allplines == processingLineCount);
    }

    vector<Topology*> topologiesToMerge;

    for (depth_group_t g : groups) {
        vector<string> gs;
        transform(g.second.begin(), g.second.end(), back_inserter(gs), [](int i) {
            return to_string(i);
        });
        cout << join(gs, ",") << endl;
    }
 
    int merge_step = 0;
    int orphan_count = 0;
    while (zones.size() > 1)
    {
        int nextRank;
        int current_depth;
        vector<zone*> to_delete;

        vector<depth_group_t> next_groups;
        if (world.rank() == 0) {
            if (groups.size() == 0) {
                cout << zones.size() << endl;
            }
            assert (groups.size() > 0);
            assert (orderedZones.size() == zones.size());

            cout << "[" << world.rank() << "] merge step " << merge_step++
                 << " (zone count: " << zones.size() << ")" << endl;

            nextRank = 0;

            get_next_groups(groups, next_groups);
            assert (next_groups.size() > 0);

            for (int gIdx = 0; gIdx < next_groups.size(); ++gIdx) {
                // broadcast a pair of <zoneId, rank> so the owner of the topology
                // can forward it to the right process
                pair<int, int> fz1 = make_pair(next_groups[gIdx].second[0], nextRank);
                pair<int, int> fz2 = make_pair(next_groups[gIdx].second[1], nextRank);
                broadcast(world, fz1, 0);
                broadcast(world, fz2, 0);
                cout << "[" << world.rank() << "] queuing join of topologies #" << fz1.first << " and " << fz2.first << endl;
                exchange_topologies(fz1, fz2, myTopologies, topologiesToMerge);

                fz1 = make_pair(next_groups[gIdx].second[2], nextRank);
                fz2 = make_pair(next_groups[gIdx].second[3], nextRank);
                broadcast(world, fz1, 0);
                broadcast(world, fz2, 0);
                cout << "[" << world.rank() << "] queuing join of topologies #" << fz1.first << " and " << fz2.first << endl;
                exchange_topologies(fz1, fz2, myTopologies, topologiesToMerge);

                if (++nextRank == world.size()) {
                    nextRank = 0;
                }

                // delay deletion of those 4 zones after merge
                for (int i = 0; i < 4; ++i) {
                    zone* z = *find_if(
                        begin(zones),
                        end(zones),
                        [next_groups,gIdx,i](const zone* z) {
                            return z->id() == next_groups[gIdx].second[i];
                        }
                    );
                    to_delete.push_back(z);
                }
            }

            // signal that we're done for this round of merging
            pair<int, int> fz1 = make_pair(-1, -1);
            broadcast(world, fz1, 0);
        }
        else {
            slave_exchange_topologies(myTopologies, topologiesToMerge);
        }

        // pair-wise merge
        vector<zone*> newZones;
        orphan_count +=
            merge_topologies(db, zones, topologiesToMerge, newZones, myTopologies);
        assert (topologiesToMerge.empty());

        vector< vector<zone*> > vz;
        gather(world, newZones, vz, 0);

        if (world.rank() == 0) {
            register_zones(vz, zones, orderedZones);
        }
        else {
            // rank 0 is the sole owner of this zones until the
            // next broadcast
            delete_all(zones);
            delete_all(newZones);
        }

        for (zone* z : to_delete) {
            zones.erase(find(begin(zones), end(zones), z));
            orderedZones.erase(find(begin(orderedZones), end(orderedZones), z));
        }
        to_delete.clear();

        broadcast(world, zones, 0);

        if (world.rank() == 0) {
            for (const zone* z : zones) {
                cout << "zone #" << z->id() << " count: " << z->count() << endl;
            }
        }
    }

    if (world.rank() == 0) {
        cout << orphan_count << " total orphans added." << endl;
        cout << "total processed lines: " << zones[0]->count() << endl;
    }

    // TODO: get topology to rank 0
    // it's in myTopologies on a random rank

    /*
    std::ofstream ofs("topology.ser");
    boost::archive::binary_oarchive oa(ofs);
    oa << mainTopology;
    */

    for (const zone* z : zones) {
        delete z;
    }
    zones.clear();

    for (zone* z : myZones) {
        delete z;
    }
    myZones.clear();

    zonesPerProcess->clear();
    delete zonesPerProcess;

    for (Topology* t : myTopologies) {
        delete t;
    }
    myTopologies.clear();

    finishGEOS();

    return 0;
}

bool slave_exchange_topologies(
    vector<Topology*>& currentTopologies,
    vector<Topology*>& newTopologies)
{
    communicator world;

    pair<int, int> fz1;
    pair<int, int> fz2;

    do {
        broadcast(world, fz1, 0);
        if (fz1.first != -1) {
            broadcast(world, fz2, 0);
            assert (fz1.second == fz2.second);
            exchange_topologies(
                fz1, fz2,
                currentTopologies, newTopologies
            );
        }
    } while (fz1.first != -1);
}

void exchange_topologies(
    pair<int, int>& fz1,
    pair<int, int>& fz2,
    vector<Topology*>& currentTopologies,
    vector<Topology*>& newTopologies)
{
    communicator world;

    vector<Topology*> send;
    for (Topology* t : currentTopologies) {
        if (t->zoneId() == fz1.first || t->zoneId() == fz2.first) {
            send.push_back(t);
        }
    }

    // proceed with exchange
    vector< vector<Topology*> > recv;
    gather(world, send, recv, fz1.second);

    // if we are the receiver, append the new topologies we just received
    if (world.rank() == fz1.second) {
        int previous_size = newTopologies.size();
        for (vector<Topology*>& v : recv) {
            if (v.size() > 0) {
                newTopologies.insert(newTopologies.end(), v.begin(), v.end());
            }
        }

        assert (newTopologies.size() == previous_size+2);

        if (newTopologies[newTopologies.size()-2]->zoneId() != fz1.first) {
            assert (newTopologies[newTopologies.size()-1]->zoneId() == fz1.first);
            assert (newTopologies[newTopologies.size()-2]->zoneId() == fz2.first);
            Topology* tmp = newTopologies[newTopologies.size()-2];
            newTopologies[newTopologies.size()-2] = newTopologies[newTopologies.size()-1];
            newTopologies[newTopologies.size()-1] = tmp;
        }
    }

    // if we are the sender, clean up the topologies we just sent
    if (send.size() > 0) {
        for (Topology* t : send) {
            assert (_is_in(t, currentTopologies));
            currentTopologies.erase(
                find(
                    begin(currentTopologies),
                    end(currentTopologies),
                    t
                )
            );
            if (world.rank() != fz1.second) {
                delete t;
            }
        }
    }
}
