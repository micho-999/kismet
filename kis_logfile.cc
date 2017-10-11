/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#include "globalregistry.h"
#include "messagebus.h"

#include "json_adapter.h"
#include "packetchain.h"

#include "kis_logfile.h"

KisLogfile::KisLogfile(GlobalRegistry *in_globalreg, std::string in_logname) :
    KisDatabase(in_globalreg, "kismetlog", in_logname) {

    std::shared_ptr<Packetchain> packetchain =
        Globalreg::FetchGlobalAs<Packetchain>(globalreg, "PACKETCHAIN");

    pack_comp_device = packetchain->RegisterPacketComponent("DEVICE");
    pack_comp_radiodata = packetchain->RegisterPacketComponent("RADIODATA");
    pack_comp_gps = packetchain->RegisterPacketComponent("GPS");
    pack_comp_linkframe = packetchain->RegisterPacketComponent("LINKFRAME");
    pack_comp_datasource = packetchain->RegisterPacketComponent("KISDATASRC");

    last_device_log = 0;

    device_stmt = NULL;
    device_pz = NULL;

    packet_stmt = NULL;
    packet_pz = NULL;

    data_stmt = NULL;
    data_pz = NULL;

    alert_stmt = NULL;
    alert_pz = NULL;

    msg_stmt = NULL;
    msg_pz = NULL;

    snapshot_stmt = NULL;
    snapshot_pz = NULL;

    Database_UpgradeDB();
}

KisLogfile::~KisLogfile() {
    local_eol_locker dblock(&ds_mutex);

    if (device_stmt != NULL)
        sqlite3_finalize(device_stmt);

    if (packet_stmt != NULL)
        sqlite3_finalize(packet_stmt);

    if (data_stmt != NULL)
        sqlite3_finalize(data_stmt);

    if (alert_stmt != NULL)
        sqlite3_finalize(alert_stmt);

    if (msg_stmt != NULL)
        sqlite3_finalize(msg_stmt);

    if (snapshot_stmt != NULL)
        sqlite3_finalize(snapshot_stmt);
}

int KisLogfile::Database_UpgradeDB() {
    local_locker dblock(&ds_mutex);

    unsigned int dbv = Database_GetDBVersion();
    std::string sql;
    int r;
    char *sErrMsg = NULL;

    if (dbv < 1) {
        sql =
            "CREATE TABLE devices ("
            
            "first_time INT, " // Time seen
            "last_time INT, "

            "phyname TEXT, " // Phy records
            "devmac TEXT, "

            "strongest_signal INT, " // Strongest signal

            "min_lat INT, " // Normalized locational bounding rectangle
            "min_lon INT, "
            "max_lat INT, "
            "max_lon INT, "

            "avg_lat INT, " // Average location
            "avg_lon INT, "

            "bytes_data INT, " // Amount of data seen on device

            "type TEXT, " // PHY specific type

            "device BLOB, " // Actual device
            
            "UNIQUE(phyname, devmac) ON CONFLICT REPLACE)";

        r = sqlite3_exec(db, sql.c_str(),
                [] (void *, int, char **, char **) -> int { return 0; }, NULL, &sErrMsg);

        if (r != SQLITE_OK) {
            _MSG("Kismet log was unable to create devices table in " + ds_dbfile + ": " +
                    std::string(sErrMsg), MSGFLAG_ERROR);
            sqlite3_close(db);
            db = NULL;
            return -1;
        }

        sql =
            "CREATE TABLE packet ("

            "ts_sec INT, " // Timestamps
            "ts_usec INT, "

            "phyname TEXT, " // Packet name and phy
            "devmac TEXT, "

            "lat INT, " // Normalized location
            "lon INT, "

            "packet_len INT, " // Packet length

            "signal INT, " // Signal level

            "datasource TEXT, " // UUID of data source

            "dlt INT, " // pcap data - datalinktype and packet bin
            "packet BLOB, "

            "error INT, " // Packet was flagged as invalid
            ")";

        r = sqlite3_exec(db, sql.c_str(),
                [] (void *, int, char **, char **) -> int { return 0; }, NULL, &sErrMsg);

        if (r != SQLITE_OK) {
            _MSG("Kismet log was unable to create packet table in " + ds_dbfile + ": " +
                    std::string(sErrMsg), MSGFLAG_ERROR);
            sqlite3_close(db);
            db = NULL;
            return -1;
        }

        sql =
            "CREATE TABLE data ("

            "ts_sec INT, " // Timestamps
            "ts_usec INT, "

            "phyname TEXT, " // Packet name and phy
            "devmac TEXT, "

            "lat INT, " // Normalized location
            "lon INT, "

            "datasource TEXT, " // UUID of data source

            "json BLOB " // Arbitrary JSON record
            ")";

        r = sqlite3_exec(db, sql.c_str(),
                [] (void *, int, char **, char **) -> int { return 0; }, NULL, &sErrMsg);

        if (r != SQLITE_OK) {
            _MSG("Kismet log was unable to create data table in " + ds_dbfile + ": " +
                    std::string(sErrMsg), MSGFLAG_ERROR);
            sqlite3_close(db);
            db = NULL;
            return -1;
        }

        sql =
            "CREATE TABLE alerts ("

            "ts_sec INT, " // Timestamps
            "ts_usec INT, "

            "phyname TEXT, " // Alert phy
            "devmac TEXT, " // Primary device associated with alert

            "lat INT, " // Normalized location
            "lon INT, "

            "header TEXT, " // Alert header/type

            "json BLOB " // Alert JSON record
            ")";

        r = sqlite3_exec(db, sql.c_str(),
                [] (void *, int, char **, char **) -> int { return 0; }, NULL, &sErrMsg);

        if (r != SQLITE_OK) {
            _MSG("Kismet log was unable to create alerts table in " + ds_dbfile + ": " +
                    std::string(sErrMsg), MSGFLAG_ERROR);
            sqlite3_close(db);
            db = NULL;
            return -1;
        }

        sql =
            "CREATE TABLE messages ("

            "ts_sec INT, " // Timestamps

            "lat INT, " // Normalized location
            "lon INT, "

            "msgtype TEXT, " // Message type
            
            "message TEXT " // message

            ")";

        r = sqlite3_exec(db, sql.c_str(),
                [] (void *, int, char **, char **) -> int { return 0; }, NULL, &sErrMsg);

        if (r != SQLITE_OK) {
            _MSG("Kismet log was unable to create messages table in " + ds_dbfile + ": " +
                    std::string(sErrMsg), MSGFLAG_ERROR);
            sqlite3_close(db);
            db = NULL;
            return -1;
        }

        sql =
            "CREATE TABLE snapshots ("

            "ts_sec INT, " // Timestamps
            "ts_usec INT, "

            "lat INT, " // Normalized location
            "lon INT, "

            "snaptype TEXT, " // Type of snapshot record

            "json BLOB " // Arbitrary record

            ")";

        r = sqlite3_exec(db, sql.c_str(),
                [] (void *, int, char **, char **) -> int { return 0; }, NULL, &sErrMsg);

        if (r != SQLITE_OK) {
            _MSG("Kismet log was unable to create messages table in " + ds_dbfile + ": " +
                    std::string(sErrMsg), MSGFLAG_ERROR);
            sqlite3_close(db);
            db = NULL;
            return -1;
        }


    }

    Database_SetDBVersion(1);

    // Prepare the statements we'll need later
    //
    sql =
        "INSERT INTO devices "
        "(first_time, last_time, phyname, devmac, strongest_signal, "
        "min_lat, min_lon, max_lat, max_lon, "
        "avg_lat, avg_lon, "
        "bytes_data, type, device) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    r = sqlite3_prepare(db, sql.c_str(), sql.length(), &device_stmt, &device_pz);

    if (r != SQLITE_OK) {
        _MSG("KisLogfile unable to prepare database insert for devices in " +
                ds_dbfile + ":" + string(sqlite3_errmsg(db)), MSGFLAG_ERROR);
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    sql =
        "INSERT INTO packet "
        "(ts_sec, ts_usec, phyname, devmac, "
        "lat, lon, "
        "packet_len, signal, "
        "datasource, "
        "dlt, packet, "
        "error) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    r = sqlite3_prepare(db, sql.c_str(), sql.length(), &packet_stmt, &packet_pz);

    if (r != SQLITE_OK) {
        _MSG("KisLogfile unable to prepare database insert for packets in " +
                ds_dbfile + ":" + string(sqlite3_errmsg(db)), MSGFLAG_ERROR);
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    sql =
        "INSERT INTO data "
        "(ts_sec, ts_usec, "
        "phyname, devmac, "
        "lat, lon, "
        "datasource, "
        "json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

    r = sqlite3_prepare(db, sql.c_str(), sql.length(), &data_stmt, &data_pz);

    if (r != SQLITE_OK) {
        _MSG("KisLogfile unable to prepare database insert for data in " +
                ds_dbfile + ":" + string(sqlite3_errmsg(db)), MSGFLAG_ERROR);
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    sql =
        "INSERT INTO alerts "
        "(ts_sec, ts_usec, phyname, devmac, "
        "lat, lon, "
        "header, "
        "json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

    r = sqlite3_prepare(db, sql.c_str(), sql.length(), &alert_stmt, &alert_pz);

    if (r != SQLITE_OK) {
        _MSG("KisLogfile unable to prepare database insert for alerts in " +
                ds_dbfile + ":" + string(sqlite3_errmsg(db)), MSGFLAG_ERROR);
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    sql =
        "INSERT INTO messages "
        "(ts_sec, "
        "lat, lon, "
        "msgtype, message) "
        "VALUES (?, ?, ?, ?, ?)";

    r = sqlite3_prepare(db, sql.c_str(), sql.length(), &msg_stmt, &msg_pz);

    if (r != SQLITE_OK) {
        _MSG("KisLogfile unable to prepare database insert for messages in " +
                ds_dbfile + ":" + string(sqlite3_errmsg(db)), MSGFLAG_ERROR);
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    sql =
        "INSERT INTO snapshots "
        "(ts_sec, ts_usec"
        "lat, lon, "
        "snaptype, json) "
        "VALUES (?, ?, ?, ?, ?, ?)";

    r = sqlite3_prepare(db, sql.c_str(), sql.length(), &snapshot_stmt, &snapshot_pz);

    if (r != SQLITE_OK) {
        _MSG("KisLogfile unable to prepare database insert for snapshots in " +
                ds_dbfile + ":" + string(sqlite3_errmsg(db)), MSGFLAG_ERROR);
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    return 1;
}

int KisLogfile::log_devices(TrackerElementVector in_devices) {
    // We avoid using external mutexes here and try to let sqlite3 handle its own
    // internal locking state; we don't want a huge device list write to block packet
    // writes for instance
    
    local_locker lock(&device_mutex);

    std::string sql;

    std::string phystring;
    std::string macstring;
    std::string typestring;

    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);
    for (auto i : in_devices) {
        sqlite3_reset(device_stmt);

        std::shared_ptr<kis_tracked_device_base> d =
            std::static_pointer_cast<kis_tracked_device_base>(d);

        phystring = d->get_phyname();
        macstring = d->get_macaddr().Mac2String();
        typestring = d->get_type_string();

        sqlite3_bind_int(device_stmt, 1, d->get_first_time());
        sqlite3_bind_int(device_stmt, 2, d->get_last_time());
        sqlite3_bind_text(device_stmt, 3, phystring.c_str(), phystring.length(), 0);
        sqlite3_bind_text(device_stmt, 4, macstring.c_str(), macstring.length(), 0);
        sqlite3_bind_int(device_stmt, 5, d->get_signal_data()->get_max_signal_dbm());

        if (d->get_tracker_location() != NULL) {
            sqlite3_bind_int(device_stmt, 6, 
                    d->get_location()->get_min_loc()->get_lat() * 100000);
            sqlite3_bind_int(device_stmt, 7,
                    d->get_location()->get_min_loc()->get_lon() * 100000);
            sqlite3_bind_int(device_stmt, 8,
                    d->get_location()->get_max_loc()->get_lat() * 100000);
            sqlite3_bind_int(device_stmt, 9,
                    d->get_location()->get_max_loc()->get_lon() * 100000);
            sqlite3_bind_int(device_stmt, 10,
                    d->get_location()->get_avg_loc()->get_lat() * 100000);
            sqlite3_bind_int(device_stmt, 11,
                    d->get_location()->get_avg_loc()->get_lon() * 100000);
        } else {
            // Empty location
            sqlite3_bind_int(device_stmt, 6, 0);
            sqlite3_bind_int(device_stmt, 7, 0);
            sqlite3_bind_int(device_stmt, 8, 0);
            sqlite3_bind_int(device_stmt, 9, 0);
            sqlite3_bind_int(device_stmt, 10, 0);
            sqlite3_bind_int(device_stmt, 11, 0);
        }

        sqlite3_bind_int(device_stmt, 12, d->get_datasize());
        sqlite3_bind_text(device_stmt, 13, typestring.c_str(), typestring.length(), 0);

        std::stringstream sstr;

        // Serialize the device
        JsonAdapter::Pack(globalreg, sstr, d, NULL);
        sqlite3_bind_text(device_stmt, 14, sstr.str().c_str(), sstr.str().length(), 0);

        sqlite3_step(device_stmt);
    }
    sqlite3_exec(db, "END TRANSACTION", NULL, NULL, NULL);

    return 1;
}

int KisLogfile::log_packet(kis_packet *in_pack) {
    local_locker lock(&packet_mutex);
    
    sqlite3_reset(packet_stmt);

    std::string phystring;
    std::string macstring;
    std::string sourceuuidstring;

    kis_datachunk *chunk = 
        (kis_datachunk *) in_pack->fetch(pack_comp_linkframe);

    kis_layer1_packinfo *radioinfo = 
        (kis_layer1_packinfo *) in_pack->fetch(pack_comp_radiodata);

    kis_gps_packinfo *gpsdata =
        (kis_gps_packinfo *) in_pack->fetch(pack_comp_gps);

    kis_tracked_device_info *devinfo =
        (kis_tracked_device_info *) in_pack->fetch(pack_comp_device);

    packetchain_comp_datasource *datasrc =
        (packetchain_comp_datasource *) in_pack->fetch(pack_comp_datasource);

    if (devinfo != NULL) {
        macstring = devinfo->devref->get_macaddr().Mac2String();
        phystring = devinfo->devref->get_phyname();
    } else {
        macstring = "00:00:00:00:00:00";
        phystring = "Unknown";
    }

    if (datasrc != NULL) {
        sourceuuidstring = datasrc->ref_source->get_source_uuid().UUID2String();
    } else {
        sourceuuidstring = "00000000-0000-0000-0000-000000000000";
    }

    sqlite3_bind_int(packet_stmt, 1, in_pack->ts.tv_sec);
    sqlite3_bind_int(packet_stmt, 2, in_pack->ts.tv_usec);

    sqlite3_bind_text(packet_stmt, 3, phystring.c_str(), phystring.length(), 0);
    sqlite3_bind_text(packet_stmt, 4, macstring.c_str(), macstring.length(), 0);

    if (gpsdata != NULL) {
        sqlite3_bind_int(packet_stmt, 5, gpsdata->lat * 100000);
        sqlite3_bind_int(packet_stmt, 6, gpsdata->lon * 100000);
    } else {
        sqlite3_bind_int(packet_stmt, 5, 0);
        sqlite3_bind_int(packet_stmt, 6, 0);
    }

    if (chunk != NULL) {
        sqlite3_bind_int(packet_stmt, 7, chunk->length);
    } else {
        sqlite3_bind_int(packet_stmt, 7, 0);
    }

    if (radioinfo != NULL) {
        sqlite3_bind_int(packet_stmt, 8, radioinfo->signal_dbm);
    } else {
        sqlite3_bind_int(packet_stmt, 8, 0);
    }

    sqlite3_bind_text(packet_stmt, 9, sourceuuidstring.c_str(), sourceuuidstring.length(), 0);

    if (chunk != NULL) {
        sqlite3_bind_int(packet_stmt, 10, chunk->dlt);
        sqlite3_bind_text(packet_stmt, 11, (const char *) chunk->data, chunk->length, 0);
    } else {
        sqlite3_bind_int(packet_stmt, 10, -1);
        sqlite3_bind_text(packet_stmt, 11, "", 0, 0);
    }

    sqlite3_bind_int(packet_stmt, 12, in_pack->error);

    sqlite3_step(packet_stmt);

    return 1;
}

int KisLogfile::log_data(kis_gps_packinfo *gps, struct timeval tv, 
        std::string phystring, mac_addr devmac, uuid datasource_uuid, 
        std::string json) {
    local_locker lock(&data_mutex);

    sqlite3_reset(data_stmt);

    std::string macstring = devmac.Mac2String();
    std::string uuidstring = datasource_uuid.UUID2String();

    sqlite3_bind_int(data_stmt, 1, tv.tv_sec);
    sqlite3_bind_int(data_stmt, 2, tv.tv_usec);

    sqlite3_bind_text(data_stmt, 3, phystring.c_str(), phystring.length(), 0);
    sqlite3_bind_text(data_stmt, 4, macstring.c_str(), macstring.length(), 0);

    if (gps != NULL) {
        sqlite3_bind_int(data_stmt, 5, gps->lat * 100000);
        sqlite3_bind_int(data_stmt, 6, gps->lon * 100000);
    } else {
        sqlite3_bind_int(data_stmt, 5, 0);
        sqlite3_bind_int(data_stmt, 6, 0);
    }

    sqlite3_bind_text(data_stmt, 7, uuidstring.c_str(), uuidstring.length(), 0);

    sqlite3_bind_text(data_stmt, 8, json.data(), json.length(), 0);

    sqlite3_step(packet_stmt);

    return 1;
}
