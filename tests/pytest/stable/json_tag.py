###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, db_test.stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-

import sys
import taos
from util.log import tdLog
from util.cases import tdCases
from util.sql import tdSql


class TDTestCase:

    def init(self, conn, logSql):
        tdLog.debug("start to execute %s" % __file__)
        tdSql.init(conn.cursor(), logSql)

    def run(self):
        tdSql.prepare()

        print("==============step1")
        tdLog.info("create database and table")
        tdSql.execute("create database db_json_tag_test")
        tdSql.execute("create table if not exists db_json_tag_test.jsons1(ts timestamp, dataInt int, dataBool bool, dataStr nchar(50)) tags(jtag json)")
        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons1_1 using db_json_tag_test.jsons1 tags('{\"loc\":\"fff\",\"id\":5}')")
        tdSql.execute("insert into db_json_tag_test.jsons1_2 using db_json_tag_test.jsons1 tags('{\"num\":5,\"location\":\"beijing\"}') values (now, 2, true, 'json2')")
        tdSql.execute("insert into db_json_tag_test.jsons1_1 values(now, 1, false, 'json1')")
        tdSql.execute("insert into db_json_tag_test.jsons1_3 using db_json_tag_test.jsons1 tags('{\"num\":34,\"location\":\"beijing\",\"level\":\"l1\"}') values (now, 3, false 'json3')")
        tdSql.execute("insert into db_json_tag_test.jsons1_4 using db_json_tag_test.jsons1 tags('{\"class\":55,\"location\":\"shanghai\",\"name\":\"name4\"}') values (now, 4, true, 'json4')")

        print("==============step2")
        tdLog.info("alter stable add tag")
        tdSql.error("ALTER STABLE db_json_tag_test.jsons1 add tag tag2 nchar(20)")

        tdSql.error("ALTER STABLE db_json_tag_test.jsons1 drop tag jtag")

        tdSql.error("ALTER TABLE db_json_tag_test.jsons1_1 SET TAG jtag=4")

        tdSql.execute("ALTER TABLE db_json_tag_test.jsons1_1 SET TAG jtag='{\"sex\":\"femail\",\"age\":35, \"isKey\":true}'")
        tdSql.query("select jtag from db_json_tag_test.jsons1_1")
        tdSql.checkData(0, 0, "{\"sex\":\"femail\",\"age\":35,\"isKey\":true}")

        print("==============step3")
        tdLog.info("select table")

        tdSql.query("select * from db_json_tag_test.jsons1")
        tdSql.checkRows(4)

        # error test
        #tdSql.error("select * from db_json_tag_test.jsons1 where jtag->'location'=4")
        tdSql.error("select * from db_json_tag_test.jsons1 where jtag->location='beijing'")
        tdSql.error("select * from db_json_tag_test.jsons1 where jtag->'location'")
        tdSql.error("select * from db_json_tag_test.jsons1 where jtag->''")
        tdSql.error("select * from db_json_tag_test.jsons1 where jtag->''=9")
        tdSql.error("select jtag->location from db_json_tag_test.jsons1")
        tdSql.error("select jtag?location from db_json_tag_test.jsons1")
        tdSql.error("select * from db_json_tag_test.jsons1 where jtag?location")
        tdSql.error("select * from db_json_tag_test.jsons1 where jtag?''")
        tdSql.error("select * from db_json_tag_test.jsons1 where jtag?'location'='beijing'")

        # test select condition
        tdSql.query("select jtag->'location' from db_json_tag_test.jsons1_2")
        tdSql.checkData(0, 0, "\"beijing\"")

        tdSql.query("select jtag->'location' from db_json_tag_test.jsons1")
        tdSql.checkRows(4)

        tdSql.query("select jtag from db_json_tag_test.jsons1_1")
        tdSql.checkRows(1)

        # test json string value
        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'location'='beijing'")
        tdSql.checkRows(2)

        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'location'!='beijing'")
        tdSql.checkRows(1)

        tdSql.query("select jtag->'num' from db_json_tag_test.jsons1 where jtag->'level'='l1'")
        tdSql.checkData(0, 0, 34)

        # test json number value
        tdSql.query("select *,tbname from db_json_tag_test.jsons1 where jtag->'class'>5 and jtag->'class'<9")
        tdSql.checkRows(0)

        tdSql.query("select *,tbname from db_json_tag_test.jsons1 where jtag->'class'>5 and jtag->'class'<92")
        tdSql.checkRows(1)

        # test where condition
        tdSql.query("select * from db_json_tag_test.jsons1 where jtag?'sex' or jtag?'num'")
        tdSql.checkRows(3)

        tdSql.query("select * from db_json_tag_test.jsons1 where jtag?'sex' or jtag?'numww'")
        tdSql.checkRows(1)

        tdSql.query("select * from db_json_tag_test.jsons1 where jtag?'sex' and jtag?'num'")
        tdSql.checkRows(0)

        tdSql.query("select jtag->'sex' from db_json_tag_test.jsons1 where jtag?'sex' or jtag?'num'")
        tdSql.checkData(0, 0, "\"femail\"")
        tdSql.checkRows(3)

        tdSql.query("select *,tbname from db_json_tag_test.jsons1 where jtag->'location'='beijing'")
        tdSql.checkRows(2)

        tdSql.query("select *,tbname from db_json_tag_test.jsons1 where jtag->'num'=5 or jtag?'sex'")
        tdSql.checkRows(2)

        # test with tbname
        tdSql.query("select * from db_json_tag_test.jsons1 where tbname = 'jsons1_1'")
        tdSql.checkRows(1)

        tdSql.query("select * from db_json_tag_test.jsons1 where tbname = 'jsons1_1' or jtag?'num'")
        tdSql.checkRows(3)

        tdSql.query("select * from db_json_tag_test.jsons1 where tbname = 'jsons1_1' and jtag?'num'")
        tdSql.checkRows(0)

        tdSql.query("select * from db_json_tag_test.jsons1 where tbname = 'jsons1_1' or jtag->'num'=5")
        tdSql.checkRows(2)

        # test where condition like
        tdSql.query("select *,tbname from db_json_tag_test.jsons1 where jtag->'location' like 'bei%'")
        tdSql.checkRows(2)

        tdSql.query("select *,tbname from db_json_tag_test.jsons1 where jtag->'location' like 'bei%' and jtag->'location'='beijin'")
        tdSql.checkRows(0)

        tdSql.query("select *,tbname from db_json_tag_test.jsons1 where jtag->'location' like 'bei%' or jtag->'location'='beijin'")
        tdSql.checkRows(2)

        tdSql.query("select *,tbname from db_json_tag_test.jsons1 where jtag->'location' like 'bei%' and jtag->'num'=34")
        tdSql.checkRows(1)

        tdSql.query("select *,tbname from db_json_tag_test.jsons1 where (jtag->'location' like 'bei%' or jtag->'num'=34) and jtag->'class'=55")
        tdSql.checkRows(0)

        tdSql.error("select * from db_json_tag_test.jsons1 where jtag->'num' like '5%'")

        # test where condition in
        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'location' in ('beijing')")
        tdSql.checkRows(2)

        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'num' in (5,34)")
        tdSql.checkRows(2)

        tdSql.error("select * from db_json_tag_test.jsons1 where jtag->'num' in ('5',34)")

        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'location' in ('shanghai') and jtag->'class'=55")
        tdSql.checkRows(1)

        # test where condition match
        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'location' match 'jin$'")
        tdSql.checkRows(0)

        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'location' match 'jin'")
        tdSql.checkRows(2)

        tdSql.query("select * from db_json_tag_test.jsons1 where datastr match 'json' and jtag->'location' match 'jin'")
        tdSql.checkRows(2)

        tdSql.error("select * from db_json_tag_test.jsons1 where jtag->'num' match '5'")

        # test json string parse
        tdSql.error("CREATE TABLE if not exists db_json_tag_test.jsons1_5 using db_json_tag_test.jsons1 tags('efwewf')")
        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons1_5 using db_json_tag_test.jsons1 tags('\t')")
        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons1_6 using db_json_tag_test.jsons1 tags('')")

        tdSql.query("select jtag from db_json_tag_test.jsons1_6")
        tdSql.checkData(0, 0, None)

        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons1_7 using db_json_tag_test.jsons1 tags('{}')")
        tdSql.query("select jtag from db_json_tag_test.jsons1_7")
        tdSql.checkData(0, 0, None)

        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons1_8 using db_json_tag_test.jsons1 tags('null')")
        tdSql.query("select jtag from db_json_tag_test.jsons1_8")
        tdSql.checkData(0, 0, None)

        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons1_9 using db_json_tag_test.jsons1 tags('{\"\":4, \"time\":null}')")
        tdSql.query("select jtag from db_json_tag_test.jsons1_9")
        tdSql.checkData(0, 0, "{\"time\":null}")
        
        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons1_10 using db_json_tag_test.jsons1 tags('{\"k1\":\"\",\"k1\":\"v1\",\"k2\":true,\"k3\":false,\"k4\":55}')")
        tdSql.query("select jtag from db_json_tag_test.jsons1_10")
        tdSql.checkData(0, 0, "{\"k1\":\"\",\"k2\":true,\"k3\":false,\"k4\":55}")

        tdSql.query("select jtag->'k2' from db_json_tag_test.jsons1_10")
        tdSql.checkData(0, 0, "true")

        tdSql.query("select jtag from db_json_tag_test.jsons1 where jtag->'k1'=''")
        tdSql.checkRows(1)

        tdSql.query("select jtag from db_json_tag_test.jsons1 where jtag->'k2'=true")
        tdSql.checkRows(1)

        tdSql.query("select jtag from db_json_tag_test.jsons1 where jtag is null")
        tdSql.checkRows(4)

        tdSql.query("select jtag from db_json_tag_test.jsons1 where jtag is not null")
        tdSql.checkRows(6)

        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'location' is not null")
        tdSql.checkRows(3)

        tdSql.query("select tbname,jtag from db_json_tag_test.jsons1 where jtag->'location' is null")
        tdSql.checkRows(7)

        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'num' is not null")
        tdSql.checkRows(2)

        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'location'='null'")
        tdSql.checkRows(0)

        tdSql.error("select * from db_json_tag_test.jsons1 where jtag->'num'='null'")

        # test distinct
        tdSql.query("select distinct jtag from db_json_tag_test.jsons1")
        tdSql.checkRows(7)

        tdSql.query("select distinct jtag->'location' from db_json_tag_test.jsons1")
        tdSql.checkRows(3)

        # test chinese
        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons1_11 using db_json_tag_test.jsons1 tags('{\"k1\":\"中国\",\"k5\":\"是是是\"}')")

        tdSql.query("select tbname,jtag from db_json_tag_test.jsons1 where jtag->'k1' match '中'")
        tdSql.checkRows(1)

        tdSql.query("select tbname,jtag from db_json_tag_test.jsons1 where jtag->'k1'='中国'")
        tdSql.checkRows(1)

        #test dumplicate key with normal colomn
        tdSql.execute("INSERT INTO db_json_tag_test.jsons1_12 using db_json_tag_test.jsons1 tags('{\"tbname\":\"tt\",\"databool\":true,\"dataStr\":\"是是是\"}') values(now, 4, false, \"你就会\")")

        tdSql.query("select *,tbname,jtag from db_json_tag_test.jsons1 where jtag->'dataStr' match '是'")
        tdSql.checkRows(1)

        tdSql.query("select tbname,jtag->'tbname' from db_json_tag_test.jsons1 where jtag->'tbname'='tt'")
        tdSql.checkRows(1)

        tdSql.query("select *,tbname,jtag from db_json_tag_test.jsons1 where dataBool=true")
        tdSql.checkRows(2)

        # test error
        tdSql.error("CREATE TABLE if not exists db_json_tag_test.jsons1_13 using db_json_tag_test.jsons1 tags(3333)")
        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons1_13 using db_json_tag_test.jsons1 tags('{\"1loc\":\"fff\",\";id\":5}')")
        tdSql.error("CREATE TABLE if not exists db_json_tag_test.jsons1_13 using db_json_tag_test.jsons1 tags('{\"。loc\":\"fff\",\"fsd\":5}')")
        tdSql.error("CREATE TABLE if not exists db_json_tag_test.jsons1_13 using db_json_tag_test.jsons1 tags('{\"试试\":\"fff\",\";id\":5}')")
        tdSql.error("insert into db_json_tag_test.jsons1_13 using db_json_tag_test.jsons1 tags(3)")
       
        # test join
        tdSql.execute("create table if not exists db_json_tag_test.jsons2(ts timestamp, dataInt int, dataBool bool, dataStr nchar(50)) tags(jtag json)")
        tdSql.execute("create table if not exists db_json_tag_test.jsons3(ts timestamp, dataInt int, dataBool bool, dataStr nchar(50)) tags(jtag json)")
        tdSql.execute("CREATE TABLE if not exists db_json_tag_test.jsons2_1 using db_json_tag_test.jsons2 tags('{\"loc\":\"fff\",\"id\":5}')")
        tdSql.execute("insert into db_json_tag_test.jsons3_1 using db_json_tag_test.jsons3 tags('{\"loc\":\"fff\",\"num\":5,\"location\":\"beijing\"}') values ('2020-04-18 15:00:00.000', 2, true, 'json2')")
        tdSql.execute("insert into db_json_tag_test.jsons2_1 values('2020-04-18 15:00:00.000', 1, false, 'json1')")
        tdSql.query("select 'sss',33,a.jtag->'loc' from db_json_tag_test.jsons2 a,db_json_tag_test.jsons3 b where a.ts=b.ts and a.jtag->'loc'=b.jtag->'loc'")
        tdSql.checkData(0, 0, "sss")
        tdSql.checkData(0, 2, "\"fff\"")
    
        # test group by & order by   string
        tdSql.query("select avg(dataint),count(*) from db_json_tag_test.jsons1 group by jtag->'location' order by jtag->'location' desc")
        tdSql.checkData(1, 0, 2.5)
        tdSql.checkData(1, 1, 2)
        tdSql.checkData(1, 2, "\"beijing\"")
        tdSql.checkData(2, 2, None)

        # test group by & order by   int
        tdSql.execute("INSERT INTO db_json_tag_test.jsons1_20 using db_json_tag_test.jsons1 tags('{\"tagint\":1}') values(now, 1, false, \"你就会\")")
        tdSql.execute("INSERT INTO db_json_tag_test.jsons1_21 using db_json_tag_test.jsons1 tags('{\"tagint\":11}') values(now, 11, false, \"你就会\")")
        tdSql.execute("INSERT INTO db_json_tag_test.jsons1_22 using db_json_tag_test.jsons1 tags('{\"tagint\":2}') values(now, 2, false, \"你就会\")")
        tdSql.query("select avg(dataint),count(*) from db_json_tag_test.jsons1 group by jtag->'tagint' order by jtag->'tagint' desc")
        tdSql.checkData(0, 0, 11)
        tdSql.checkData(0, 2, 11)
        tdSql.checkData(1, 2, 2)
        tdSql.checkData(3, 2, None)
        tdSql.checkData(3, 1, 5)
        tdSql.query("select avg(dataint),count(*) from db_json_tag_test.jsons1 group by jtag->'tagint' order by jtag->'tagint'")
        tdSql.checkData(0, 1, 5)
        tdSql.checkData(0, 2, None)
        tdSql.checkData(1, 2, 1)
        tdSql.checkData(3, 2, 11)

        # test json->'key'=null
        tdSql.execute("insert into db_json_tag_test.jsons1_9 values('2020-04-17 15:20:00.000', 5, false, 'json19')")
        tdSql.query("select * from db_json_tag_test.jsons1")
        tdSql.checkRows(9)
        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'time' is null")
        tdSql.checkRows(8)
        tdSql.query("select * from db_json_tag_test.jsons1 where jtag->'time'=null")
        tdSql.checkRows(1)

    def stop(self):
        tdSql.close()
        tdLog.success("%s successfully executed" % __file__)


tdCases.addWindows(__file__, TDTestCase())
tdCases.addLinux(__file__, TDTestCase())
