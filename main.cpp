#include <ctime>
#include <chrono>
#include <cassert>
#include <cstdlib>
#include <utility>
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

#include <boost/program_options.hpp>

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

namespace po = boost::program_options;

namespace cma {
    GEOSContextHandle_t hdl;
}

bool slave_exchange_topologies(
    GEOSHelper* geos,
    vector<int>& newTopologies);

void exchange_topologies(
    GEOSHelper* geos,
    pair<zone*, int>& fz1,
    pair<zone*, int>& fz2,
    vector<int>& newTopologies);

int main(int argc, char **argv)
{
    environment env;
    communicator world;

    int ret = -1;
    bool merge_only = false;
    bool restore = true;
    int first_merge_step = 0;
    string postgres_connect_str;
    po::variables_map vm;
    if (world.rank() == 0) {
        po::options_description desc("Allowed options");
        desc.add_options()
            ("help", "Produce help message")
            ("db", po::value<string>()->required(), "PostgreSQL connect string (required)")
            ("merge-only", "Skip to merge phase (default: 0/false)")
            ("no-merge-restore", "Don't restore merged topologies (default: restore)")
            ("merge-step", po::value<int>()->default_value(0), "Merge step to resume (default: 0/all steps)")
        ;

        try {
            po::store(po::parse_command_line(argc, argv, desc), vm);
            po::notify(vm);

            if (vm.count("help")) {
                cout << desc << endl;
                ret = 1;
            }
            else {
                merge_only = vm.count("merge-only");
                restore = !vm.count("no-merge-restore");
                postgres_connect_str = vm["db"].as<string>();
                first_merge_step = vm["merge-step"].as<int>();
            }
        } catch (const po::required_option&) {
            cerr << desc << endl;
            ret = 1;
        }
    }

    broadcast(world, ret, 0);
    if (ret > 0) {
        return ret;
    }

    broadcast(world, restore, 0);
    broadcast(world, merge_only, 0);
    broadcast(world, first_merge_step, 0);
    broadcast(world, postgres_connect_str, 0);

    initGEOS(geos_message_function, geos_message_function);
    OGRRegisterAll();

    unique_ptr<GEOSHelper> geos(new GEOSHelper());
    assert (hdl != NULL);

    geom_container::s_wkbr = GEOSWKBReader_create();

    PG db(postgres_connect_str);
    if (!db.connected()) {
        cerr << "Could not connect to PostgreSQL." << endl;
        return 1;
    }

    int line_count;
    if (world.rank() == 0) {
        line_count = db.get_line_count();
    }
    broadcast(world, line_count, 0);
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

        if (!restore_zones(zones, groups)) {
            prepare_zones(postgres_connect_str, *geos, world_extent, zones, groups, 20);
            save_zones(zones, groups);
        }
        assert (!zones.empty());
        assert (!groups.empty());

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

        cout << "Will process " << processingLineCount << ", leaving " << line_count-processingLineCount
             << " orphans (" << (line_count-processingLineCount)/float(processingLineCount)*100 << "%)" << endl;
    }

    list<zone*> myZones;
    if (!merge_only) {
        scatter(world, *processZones, myZones, 0);
    }

    processZones->clear();

    delete processZones;

    int pline = 0;
    for (zone* z : myZones) {
        int zoneId = z->id();
        const string& hexWKT = geos->as_string(z->geom());

        chrono::time_point<chrono::system_clock> start, end;
        start = chrono::system_clock::now();

        Topology* topology = restore_topology(geos.get(), z, false);
        if (topology) {
            /*
            if (topology->count() != lines.size()) {
                cerr << "restored topology for zone #" << topology->zoneId() << " contains "
                     << topology->count() << " roads but should contain " << lines.size()
                     << " according to the database." << endl;
                exit(1);
            }
            */
            cout << "[" << world.rank() << "] topology for zone #" << zoneId
                 << " has been restored from a checkpoint." << endl;
            pline += topology->count();
            delete topology;
            continue;
        }

        GEOSGeometry* zoneGeom =
            GEOSWKTReader_read_r(hdl, geos->text_reader(), hexWKT.c_str());

        linesV lines;
        if (!db.get_lines(zoneGeom, lines, true)) {
            assert (false);
        }

        pline += lines.size();

        GEOSGeom_destroy_r(hdl, zoneGeom);

        cout << "[" << world.rank() << "] processing zone #" <<  zoneId
             << " (" << lines.size() << ") lines." << endl;

        if (lines.size() == 0) {
            Topology* topology = new Topology(geos.get());
            topology->zoneId(z->id());
            save_topology(geos.get(), z, topology);
            delete topology;
            continue;
        }

        topology = new Topology(geos.get());
        topology->zoneId(z->id());

        int lc = 0;
        for (pair<int, GEOSGeometry*>& line_info : lines) {
            int lineId = line_info.first;
            GEOSGeometry* line = line_info.second;
            try {
                topology->TopoGeo_AddLineString(lineId, line, DEFAULT_TOLERANCE);
                topology->commit();
            }
            catch (const runtime_error& ex) {
                cerr << "Line #" << topology->count() << " - " << geos->as_string(line) << ": " << ex.what() << endl;
                cerr << "Cannot complete topology for zone id #" << topology->zoneId() << endl;
                topology->rollback();
                delete topology;
                topology = new Topology();
                topology->zoneId(z->id());
                break;
            }
            catch (const invalid_argument& ex) {
                cerr << "Line #" << topology->count() << " - " << geos->as_string(line) << ": " << ex.what() << endl;
                topology->rollback();
            }

            GEOSGeom_destroy_r(hdl, line);

            if (++lc % 100 == 0) {
                // cout << lc << endl;
            }
        }
        lines.clear();

        end = chrono::system_clock::now();
        chrono::duration<double> elapsed_seconds = end-start;
        time_t end_time = chrono::system_clock::to_time_t(end);

        char* t = std::ctime(&end_time); t[strlen(t)-2] = '\0';
        cout << "[" << world.rank() << "] finished computation of zone #" << zoneId
             << " at " << t << ","
             << " elapsed time: " << elapsed_seconds.count() << "s" << endl;

        save_topology(geos.get(), z, topology);
        delete topology;
    }

    broadcast(world, zones, 0);

    vector<int> topologiesToMerge;

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

        chrono::time_point<chrono::system_clock> _start, _end;
        _start = chrono::system_clock::now();

        vector<depth_group_t> next_groups;
        if (world.rank() == 0) {
            assert (groups.size() > 0);
            assert (orderedZones.size() == zones.size());

            get_next_groups(groups, next_groups);
            assert (next_groups.size() > 0);

            if (merge_step < first_merge_step)
            {
                /**
                 * If we skip a step, we need to compute the merged zones envelope.
                 */
                cout << "[" << world.rank() << "] skipping merge step " << merge_step << endl;
                for (int gIdx = 0; gIdx < next_groups.size(); ++gIdx) {

                    int mergedZoneId = next_groups[gIdx].second[0];
                    OGREnvelope envelope = get_zone_by_id(zones, next_groups[gIdx].second[0])->envelope();

                    assert (next_groups[gIdx].second.size() == 4);

                    to_delete.clear();
                    for (int i = 0; i < 4; ++i) {
                        if (i > 0) {
                            envelope.Merge(get_zone_by_id(zones, next_groups[gIdx].second[i])->envelope());
                        }

                        zone* z = *find_if(
                            begin(zones),
                            end(zones),
                            [next_groups,gIdx,i](const zone* z) {
                                return z->id() == next_groups[gIdx].second[i];
                            }
                        );

                        to_delete.push_back(z);
                    }

                    zone* merged_zone = new zone(mergedZoneId, envelope);

                    orderedZones.insert(
                        find_if(
                            begin(orderedZones),
                            end(orderedZones),
                            [merged_zone](const zone* a) {
                                return merged_zone->id() == a->id();
                            }
                        ),
                        merged_zone
                    );
                    zones.push_back(merged_zone);

                    for (zone* z : to_delete) {
                        zones.erase(find(begin(zones), end(zones), z));
                        orderedZones.erase(find(begin(orderedZones), end(orderedZones), z));
                    }
                    to_delete.clear();
                }

                broadcast(world, zones, 0);
                ++merge_step;
                continue;
            }

            cout << "[" << world.rank() << "] merge step " << merge_step++
                 << " (zone count: " << zones.size() << ", group count: "
                 << next_groups.size() << ")" << endl;

            nextRank = 0;

            for (int gIdx = 0; gIdx < next_groups.size(); ++gIdx) {
                // broadcast a pair of <zone*, rank> so the right rank can load it from disk
                pair<zone*, int> fz1 = make_pair(
                    get_zone_by_id(zones, next_groups[gIdx].second[0]),
                    nextRank);
                pair<zone*, int> fz2 = make_pair(
                    get_zone_by_id(zones, next_groups[gIdx].second[1]),
                    nextRank);
                broadcast(world, fz1, 0);
                broadcast(world, fz2, 0);
                cout << "[" << world.rank() << "] queuing join of topologies #" << fz1.first->id() << " and " << fz2.first->id() << endl;
                exchange_topologies(geos.get(), fz1, fz2, topologiesToMerge);

                fz1 = make_pair(
                    get_zone_by_id(zones, next_groups[gIdx].second[2]),
                    nextRank);
                fz2 = make_pair(
                    get_zone_by_id(zones, next_groups[gIdx].second[3]),
                    nextRank);
                broadcast(world, fz1, 0);
                broadcast(world, fz2, 0);
                cout << "[" << world.rank() << "] queuing join of topologies #" << fz1.first->id() << " and " << fz2.first->id() << endl;
                exchange_topologies(geos.get(), fz1, fz2, topologiesToMerge);

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
            pair<zone*, int> fz1 = make_pair(nullptr, -1);
            broadcast(world, fz1, 0);
        }
        else {
            if (merge_step < first_merge_step) {
                delete_all(zones);
                broadcast(world, zones, 0);
                ++merge_step;
                continue;
            }

            slave_exchange_topologies(geos.get(), topologiesToMerge);
        }

        // pair-wise merge
        vector<zone*> newZones;
        orphan_count +=
            merge_topologies(db, geos.get(), zones, topologiesToMerge, newZones, restore);
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

            _end = chrono::system_clock::now();
            chrono::duration<double> elapsed_seconds = _end-_start;
            time_t end_time = chrono::system_clock::to_time_t(_end);

            char* t = std::ctime(&end_time); t[strlen(t)-2] = '\0';
            cout << "[" << world.rank() << "] merge step " << (merge_step-1)
                 << " at " << t << ","
                 << " elapsed time: " << elapsed_seconds.count() << "s" << endl;
        }
    }

    if (world.rank() == 0) {
        cout << orphan_count << " total orphans added." << endl;
        cout << "total processed lines: " << zones[0]->count() << endl;
    }

    assert (zones.size() == 1);
    if (world.rank() == 0) {
        Topology *topology = restore_topology(geos.get(), zones[0], false);
        std::ofstream ofs("topology.ser");
        boost::archive::binary_oarchive oa(ofs);
        oa << *topology;
        delete topology;
    }

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

    finishGEOS();

    return 0;
}

bool slave_exchange_topologies(
    GEOSHelper* geos,
    vector<int>& newTopologies)
{
    communicator world;

    pair<zone*, int> fz1;
    pair<zone*, int> fz2;

    do {
        broadcast(world, fz1, 0);
        if (fz1.first != nullptr) {
            broadcast(world, fz2, 0);
            assert (fz1.second == fz2.second);

            exchange_topologies(
                geos,
                fz1, fz2,
                newTopologies
            );
        }
    } while (fz1.first != nullptr);
}

void exchange_topologies(
    GEOSHelper* geos,
    pair<zone*, int>& fz1,
    pair<zone*, int>& fz2,
    vector<int>& newTopologies)
{
    communicator world;

    if (world.rank() == fz1.second) {
        newTopologies.push_back(fz1.first->id());
        newTopologies.push_back(fz2.first->id());

        if (newTopologies[newTopologies.size()-2] != fz1.first->id()) {
            assert (newTopologies[newTopologies.size()-1] == fz1.first->id());
            assert (newTopologies[newTopologies.size()-2] == fz2.first->id());
            swap(
                newTopologies[newTopologies.size()-1],
                newTopologies[newTopologies.size()-2]
            );
        }
    }
}
