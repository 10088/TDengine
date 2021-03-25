###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-

import sys
import os
from util.log import *
from util.cases import *
from util.sql import *
from util.dnodes import *


class TDTestCase:
    def init(self, conn, logSql):
        tdLog.debug("start to execute %s" % __file__)
        tdSql.init(conn.cursor(), logSql)
        
    def getBuildPath(self):
        selfPath = os.path.dirname(os.path.realpath(__file__))

        if ("community" in selfPath):
            projPath = selfPath[:selfPath.find("community")]
        else:
            projPath = selfPath[:selfPath.find("tests")]

        for root, dirs, files in os.walk(projPath):
            if ("taosd" in files):
                rootRealPath = os.path.dirname(os.path.realpath(root))
                if ("packaging" not in rootRealPath):
                    buildPath = root[:len(root)-len("/build/bin")]
                    break
        return buildPath
        
    def run(self):
        tdSql.prepare()
        buildPath = self.getBuildPath()
        if (buildPath == ""):
            tdLog.exit("taosd not found!")
        else:
            tdLog.info("taosd found in %s" % buildPath)
        binPath = buildPath+ "/build/bin/"
        # insert: create one  and mutiple tables per sql and insert multiple rows per sql 
        os.system("yes | %staosdemo -f tools/taosdemoAllTest/insert-1s1tntnr.json" % binPath)
        tdSql.execute("use db")
        tdSql.query("show stables")
        tdSql.checkData(0, 4, 10)
        tdSql.query("select count(*) from s_0_tb01")
        tdSql.checkData(0, 0, 2000)   

        # insert: create one table per sql and insert multiple rows
        # os.system("yes | %staosdemo -f tools/taosdemoAllTest/insert-1s1tnr.json" % binPath)
        # tdSql.execute("use db01")
        # tdSql.query("show stables")
        # tdSql.checkData(0, 4, 10)
        # tdSql.query("select count(*) from stb01")
        # tdSql.checkData(0, 0, 2000)   
        # # insert: create one table per sql and insert multiple rows
        # os.system("yes | %staosdemo -f tools/taosdemoAllTest/insert-1snt1r.json" % binPath)
        # tdSql.execute("use db01")
        # tdSql.query("show stables")
        # tdSql.checkData(0, 4, 10)
        # tdSql.query("select count(*) from stb01")
        # tdSql.checkData(0, 0, 2000) 
        # # insert: create one table per sql and insert multiple rows
        # os.system("yes | %staosdemo -f tools/taosdemoAllTest/insert-1sntnr.json" % binPath)
        # tdSql.execute("use db01")
        # tdSql.query("show stables")
        # tdSql.checkData(0, 4, 10)
        # tdSql.query("select count(*) from stb01")
        # tdSql.checkData(0, 0, 2000)     
        # # insert: inser unlimted_speed .json
        # os.system("yes | %staosdemo -f tools/taosdemoAllTest//insert-highspeed.json" % binPath)
        # tdSql.execute("use db01")
        # tdSql.query("show stables")
        # tdSql.checkData(0, 4, 10)
        # tdSql.query("select count(*) from stb01")
        # tdSql.checkData(0, 0, 200000)   


    def stop(self):
        tdSql.close()
        tdLog.success("%s successfully executed" % __file__)


tdCases.addWindows(__file__, TDTestCase())
tdCases.addLinux(__file__, TDTestCase())
