# coding: utf-8

# Copyright (C) 1994-2019 Altair Engineering, Inc.
# For more information, contact Altair at www.altair.com.
#
# This file is part of the PBS Professional ("PBS Pro") software.
#
# Open Source License Information:
#
# PBS Pro is free software. You can redistribute it and/or modify it under the
# terms of the GNU Affero General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version.
#
# PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.
# See the GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# Commercial License Information:
#
# For a copy of the commercial license terms and conditions,
# go to: (http://www.pbspro.com/UserArea/agreement.html)
# or contact the Altair Legal Department.
#
# Altair’s dual-license business model allows companies, individuals, and
# organizations to create proprietary derivative works of PBS Pro and
# distribute them - whether embedded or bundled with other software -
# under a commercial license agreement.
#
# Use of Altair’s trademarks, including but not limited to "PBS™",
# "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
# trademark licensing policies.

from tests.functional import *


class TestEntityLimits(TestFunctional):

    """
    This test suite tests working of queued_jobs_threshold, max_queued,
    queued_jobs_threshold_res and max_queued_res.

    PBS supports entity limits at queue and server level. And these limits
    can be applied for a user, group, project or overall.
    This test suite iterates over all the entities.

    """

    limit = 10

    def setUp(self):
        TestFunctional.setUp(self)

        a = {'resources_available.ncpus': 1}
        self.server.manager(MGR_CMD_SET, NODE, a, self.mom.shortname)

    def common_limit_test(self, server, entstr, job_attr={}, queued=False,
                          exp_err=''):
        if not server:
            qname = self.server.default_queue
            self.server.manager(MGR_CMD_SET, QUEUE, entstr, qname)
        else:
            self.server.manager(MGR_CMD_SET, SERVER, entstr)

        if queued:
            j = Job(TEST_USER, job_attr)
            jid = self.server.submit(j)
            self.server.expect(JOB, {'job_state': 'R'}, id=jid)

        for _ in range(self.limit):
            j = Job(TEST_USER, job_attr)
            self.server.submit(j)

        try:
            j = Job(TEST_USER, job_attr)
            self.server.submit(j)
        except PbsSubmitError as e:
            if e.msg[0] != exp_err:
                raise self.failureException("rcvd unexpected err message: " +
                                            e.msg[0])
        else:
            self.assertFalse(True, "Job violating limits got submitted.")

        self.server.cleanup_jobs()

        try:
            jval = "1-" + str(self.limit + 1)
            job_attr[ATTR_J] = jval
            j = Job(TEST_USER, job_attr)
            jid = self.server.submit(j)
        except PbsSubmitError as e:
            if e.msg[0] != exp_err:
                raise self.failureException("rcvd unexpected err message: " +
                                            e.msg[0])
        else:
            self.assertFalse(True, "Array Job violating limits got submitted.")

        jval = "1-" + str(self.limit)
        job_attr[ATTR_J] = jval

        j = Job(TEST_USER, job_attr)
        jid = self.server.submit(j)
        self.server.expect(JOB, {'job_state': 'B'}, id=jid)

        del job_attr[ATTR_J]

        if queued:
            j = Job(TEST_USER, job_attr)
            self.server.submit(j)

        try:
            j = Job(TEST_USER, job_attr)
            self.server.submit(j)
        except PbsSubmitError as e:
            if e.msg[0] != exp_err:
                raise self.failureException("rcvd unexpected err message: " +
                                            e.msg[0])
        else:
            self.assertFalse(True, "Job violating limits got submitted.")

        self.server.restart()

        try:
            self.server.submit(j)
        except PbsSubmitError as e:
            if e.msg[0] != exp_err:
                raise self.failureException("rcvd unexpected err message: " +
                                            e.msg[0])
        else:
            self.assertFalse(True, "Job violating limits got submitted after "
                             "server restart.")

    def test_server_generic_user_limits_queued(self):
        """
        Test queued_jobs_threshold for any user at the server level.
        """
        a = {"queued_jobs_threshold":
             "[u:PBS_GENERIC=" + str(self.limit) + "]"}
        m = "qsub: would exceed complex's per-user limit of jobs in 'Q' state"
        self.common_limit_test(True, a, queued=True, exp_err=m)

    def test_server_user_limits_queued(self):
        """
        Test queued_jobs_threshold for user TEST_USER at the server level.
        """
        a = {"queued_jobs_threshold":
             "[u:" + str(TEST_USER) + "=" + str(self.limit) + "]"}
        errmsg = "qsub: Maximum number of jobs in 'Q' state for user " + \
            str(TEST_USER) + ' already in complex'
        self.common_limit_test(True, a, queued=True, exp_err=errmsg)

    def test_server_project_limits_queued(self):
        """
        Test queued_jobs_threshold for project p1 at the server level.
        """
        a = {"queued_jobs_threshold": "[p:p1=" + str(self.limit) + "]"}
        attrs = {ATTR_project: 'p1'}
        errmsg = "qsub: Maximum number of jobs in 'Q' state for project p1 " \
            + "already in complex"
        self.common_limit_test(True, a, attrs, queued=True, exp_err=errmsg)

    def test_server_generic_project_limits_queued(self):
        """
        Test queued_jobs_threshold for any project at the server level.
        """
        a = {"queued_jobs_threshold":
             "[p:PBS_GENERIC=" + str(self.limit) + "]"}
        errmsg = "qsub: would exceed complex's per-project limit of jobs in " \
            + "'Q' state"
        self.common_limit_test(True, a, queued=True, exp_err=errmsg)

    @skipOnShasta
    def test_server_group_limits_queued(self):
        """
        Test queued_jobs_threshold for group TSTGRP0 at the server level.
        """
        a = {"queued_jobs_threshold":
             "[g:" + str(TSTGRP0) + "=" + str(self.limit) + "]"}
        errmsg = "qsub: Maximum number of jobs in 'Q' state for group " + \
            str(TSTGRP0) + ' already in complex'
        self.common_limit_test(True, a, queued=True, exp_err=errmsg)

    @skipOnShasta
    def test_server_generic_group_limits_queued(self):
        """
        Test queued_jobs_threshold for any group at the server level.
        """
        a = {"queued_jobs_threshold":
             "[g:PBS_GENERIC=" + str(self.limit) + "]"}
        m = "qsub: would exceed complex's per-group limit of jobs in 'Q' state"
        self.common_limit_test(True, a, queued=True, exp_err=m)

    def test_server_overall_limits_queued(self):
        """
        Test queued_jobs_threshold for any entity at the server level.
        """
        a = {"queued_jobs_threshold": "[o:PBS_ALL=" + str(self.limit) + "]"}
        errmsg = "qsub: Maximum number of jobs in 'Q' state already in complex"
        self.common_limit_test(True, a, queued=True, exp_err=errmsg)

    def test_queue_generic_user_limits_queued(self):
        """
        Test queued_jobs_threshold for any user for the default queue.
        """
        a = {"queued_jobs_threshold":
             "[u:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = "qsub: would exceed queue generic's per-user limit of " \
            + "jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    def test_queue_user_limits_queued(self):
        """
        Test queued_jobs_threshold for user pbsuser1 for the default queue.
        """
        a = {"queued_jobs_threshold":
             "[u:" + str(TEST_USER) + "=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = "qsub: Maximum number of jobs in 'Q' state for user " + \
            str(TEST_USER) + ' already in queue ' + self.server.default_queue
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    @skipOnShasta
    def test_queue_group_limits_queued(self):
        """
        Test queued_jobs_threshold for group TSTGRP0 for the default queue.
        """
        a = {"queued_jobs_threshold":
             "[g:" + str(TSTGRP0) + "=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = "qsub: Maximum number of jobs in 'Q' state for group " + \
            str(TSTGRP0) + ' already in queue ' + self.server.default_queue
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    def test_queue_project_limits_queued(self):
        """
        Test queued_jobs_threshold for project p1 for the default queue.
        """
        a = {"queued_jobs_threshold": "[p:p1=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue, ATTR_project: 'p1'}
        errmsg = "qsub: Maximum number of jobs in 'Q' state for project p1 " \
            'already in queue ' + self.server.default_queue
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    def test_queue_generic_project_limits_queued(self):
        """
        Test queued_jobs_threshold for any project for the default queue.
        """
        a = {"queued_jobs_threshold":
             "[p:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = 'qsub: would exceed queue ' + self.server.default_queue + \
            "'s per-project limit of jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    @skipOnShasta
    def test_queue_generic_group_limits_queued(self):
        """
        Test queued_jobs_threshold for any group for the default queue.
        """
        a = {"queued_jobs_threshold":
             "[g:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = 'qsub: would exceed queue ' + self.server.default_queue + \
            "'s per-group limit of jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    def test_queue_overall_limits_queued(self):
        """
        Test queued_jobs_threshold for all entities for the default queue.
        """
        a = {"queued_jobs_threshold": "[o:PBS_ALL=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        emsg = "qsub: Maximum number of jobs in 'Q' state already in queue " \
            + self.server.default_queue
        self.common_limit_test(False, a, attrs, queued=True, exp_err=emsg)

    def test_server_generic_user_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for any user at the server level.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[u:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed per-user limit on resource ncpus in ' \
            + "complex for jobs in 'Q' state"
        self.common_limit_test(True, a, attrs, queued=True, exp_err=errmsg)

    def test_server_user_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for user pbsuser1 at the server level.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[u:" + str(TEST_USER) + "=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed user ' + str(TEST_USER) + \
            "'s limit on resource ncpus in complex for jobs in 'Q' state"
        self.common_limit_test(True, a, attrs, queued=True, exp_err=errmsg)

    @skipOnShasta
    def test_server_generic_group_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for any group at the server level.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[g:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed per-group limit on resource ncpus in '\
            + "complex for jobs in 'Q' state"
        self.common_limit_test(True, a, attrs, queued=True, exp_err=errmsg)

    def test_server_project_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for project p1 at the server level.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[p:p1=" + str(self.limit) + "]"}
        attrs = {ATTR_project: 'p1', 'Resource_List.select': '1:ncpus=1'}
        errmsg = "qsub: would exceed project p1's limit on resource ncpus in" \
            + " complex for jobs in 'Q' state"
        self.common_limit_test(True, a, attrs, queued=True, exp_err=errmsg)

    def test_server_generic_project_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for any project at the server level.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[p:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed per-project limit on resource ncpus in ' \
            + "complex for jobs in 'Q' state"
        self.common_limit_test(True, a, attrs, queued=True, exp_err=errmsg)

    @skipOnShasta
    def test_server_group_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for group pbsuser1 at the server level.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[g:" + str(TSTGRP0) + "=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed group ' + str(TSTGRP0) + \
            "'s limit on resource ncpus in complex for jobs in 'Q' state"
        self.common_limit_test(True, a, attrs, queued=True, exp_err=errmsg)

    def test_server_overall_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for all entities at the server level.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[o:PBS_ALL=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed limit on resource ncpus in complex for '\
            + "jobs in 'Q' state"
        self.common_limit_test(True, a, attrs, queued=True, exp_err=errmsg)

    def test_queue_generic_user_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for all users for the default queue.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[u:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        emsg = 'qsub: would exceed per-user limit on resource ncpus in queue '\
            + self.server.default_queue + " for jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=emsg)

    def test_queue_user_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for user pbsuser1 for the default queue.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[u:" + str(TEST_USER) + "=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed user ' + str(TEST_USER) + \
            "'s limit on resource ncpus in queue " + \
            self.server.default_queue + " for jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    @skipOnShasta
    def test_queue_group_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for group pbsuser1 for the default queue
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[g:" + str(TSTGRP0) + "=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed group ' + str(TSTGRP0) + \
            "'s limit on resource ncpus in queue " + \
            self.server.default_queue + " for jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    @skipOnShasta
    def test_queue_generic_group_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for any group for the default queue.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[g:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed per-group limit on resource ncpus in ' \
            + 'queue ' + self.server.default_queue + " for jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    def test_queue_project_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for project p1 for the default queue.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[p:p1=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 ATTR_project: 'p1', 'Resource_List.select': '1:ncpus=1'}
        errmsg = "qsub: would exceed project p1's limit on resource ncpus " + \
            'in queue ' + self.server.default_queue + " for jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    def test_queue_generic_project_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for any project for the default queue.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[p:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed per-project limit on resource ncpus in' \
            + ' queue ' + self.server.default_queue + " for jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    def test_queue_overall_limits_res_queued(self):
        """
        Test queued_jobs_threshold_res for any entity for the default queue.
        """
        a = {"queued_jobs_threshold_res.ncpus":
             "[o:PBS_ALL=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed limit on resource ncpus in queue ' + \
            self.server.default_queue + " for jobs in 'Q' state"
        self.common_limit_test(False, a, attrs, queued=True, exp_err=errmsg)

    def test_server_generic_user_limits_max(self):
        """
        Test max_queued for any user at the server level.
        """
        a = {"max_queued":
             "[u:PBS_GENERIC=" + str(self.limit) + "]"}
        errmsg = "qsub: would exceed complex's per-user limit"
        self.common_limit_test(True, a, exp_err=errmsg)

    def test_server_user_limits_max(self):
        """
        Test max_queued for user pbsuser1 at the server level.
        """
        a = {"max_queued":
             "[u:" + str(TEST_USER) + "=" + str(self.limit) + "]"}
        errmsg = 'qsub: Maximum number of jobs for user ' + str(TEST_USER) + \
            ' already in complex'
        self.common_limit_test(True, a, exp_err=errmsg)

    def test_server_project_limits_max(self):
        """
        Test max_queued for project p1 at the server level.
        """
        a = {"max_queued": "[p:p1=" + str(self.limit) + "]"}
        attrs = {ATTR_project: 'p1'}
        msg = 'qsub: Maximum number of jobs for project p1 already in complex'
        self.common_limit_test(True, a, attrs, exp_err=msg)

    def test_server_generic_project_limits_max(self):
        """
        Test max_queued for any project at the server level.
        """
        a = {"max_queued":
             "[p:PBS_GENERIC=" + str(self.limit) + "]"}
        errmsg = "qsub: would exceed complex's per-project limit"
        self.common_limit_test(True, a, exp_err=errmsg)

    @skipOnShasta
    def test_server_group_limits_max(self):
        """
        Test max_queued for group TSTGRP0 at the server level.
        """
        a = {"max_queued":
             "[g:" + str(TSTGRP0) + "=" + str(self.limit) + "]"}
        errmsg = 'qsub: Maximum number of jobs for group ' + str(TSTGRP0) + \
            ' already in complex'
        self.common_limit_test(True, a, exp_err=errmsg)

    @skipOnShasta
    def test_server_generic_group_limits_max(self):
        """
        Test max_queued for any group at the server level.
        """
        a = {"max_queued":
             "[g:PBS_GENERIC=" + str(self.limit) + "]"}
        errmsg = "qsub: would exceed complex's per-group limit"
        self.common_limit_test(True, a, exp_err=errmsg)

    def test_server_overall_limits_max(self):
        """
        Test max_queued for any entity at the server level.
        """
        a = {"max_queued": "[o:PBS_ALL=" + str(self.limit) + "]"}
        errmsg = 'qsub: Maximum number of jobs already in complex'
        self.common_limit_test(True, a, exp_err=errmsg)

    def test_queue_generic_user_limits_max(self):
        """
        Test max_queued for any user for the default queue.
        """
        a = {"max_queued":
             "[u:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = "qsub: would exceed queue generic's per-user limit"
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    def test_queue_user_limits_max(self):
        """
        Test max_queued for user pbsuser1 for the default queue.
        """
        a = {"max_queued":
             "[u:" + str(TEST_USER) + "=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = 'qsub: Maximum number of jobs for user ' + str(TEST_USER) + \
            ' already in queue ' + self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    @skipOnShasta
    def test_queue_group_limits_max(self):
        """
        Test max_queued for group pbsuser1 for the default queue.
        """
        a = {"max_queued":
             "[g:" + str(TSTGRP0) + "=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = 'qsub: Maximum number of jobs for group ' + str(TSTGRP0) + \
            ' already in queue ' + self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    def test_queue_project_limits_max(self):
        """
        Test max_queued for project p1 for the default queue.
        """
        a = {"max_queued": "[p:p1=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue, ATTR_project: 'p1'}
        msg = 'qsub: Maximum number of jobs for project p1 already in queue '\
            + self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=msg)

    def test_queue_generic_project_limits_max(self):
        """
        Test max_queued for any project for the default queue.
        """
        a = {"max_queued":
             "[p:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = 'qsub: would exceed queue ' + self.server.default_queue + \
            "'s per-project limit"
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    @skipOnShasta
    def test_queue_generic_group_limits_max(self):
        """
        Test max_queued for any group for the default queue.
        """
        a = {"max_queued":
             "[g:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = 'qsub: would exceed queue ' + self.server.default_queue + \
            "'s per-group limit"
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    def test_queue_overall_limits_max(self):
        """
        Test max_queued for all entities for the default queue.
        """
        a = {"max_queued": "[o:PBS_ALL=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue}
        errmsg = 'qsub: Maximum number of jobs already in queue ' + \
            self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    def test_server_generic_user_limits_res_max(self):
        """
        Test max_queued_res for any user at the server level.
        """
        a = {"max_queued_res.ncpus":
             "[u:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        emsg = 'qsub: would exceed per-user limit on resource ncpus in complex'
        self.common_limit_test(True, a, attrs, exp_err=emsg)

    def test_server_user_limits_res_max(self):
        """
        Test max_queued_res for user pbsuser1 at the server level.
        """
        a = {"max_queued_res.ncpus":
             "[u:" + str(TEST_USER) + "=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed user ' + str(TEST_USER) + \
            "'s limit on resource ncpus in complex"
        self.common_limit_test(True, a, attrs, exp_err=errmsg)

    @skipOnShasta
    def test_server_generic_group_limits_res_max(self):
        """
        Test max_queued_res for any group at the server level.
        """
        a = {"max_queued_res.ncpus":
             "[g:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        msg = 'qsub: would exceed per-group limit on resource ncpus in complex'
        self.common_limit_test(True, a, attrs, exp_err=msg)

    def test_server_project_limits_res_max(self):
        """
        Test max_queued_res for project p1 at the server level.
        """
        a = {"max_queued_res.ncpus":
             "[p:p1=" + str(self.limit) + "]"}
        attrs = {ATTR_project: 'p1', 'Resource_List.select': '1:ncpus=1'}
        errmsg = "qsub: would exceed project p1's limit on resource ncpus in" \
            + " complex"
        self.common_limit_test(True, a, attrs, exp_err=errmsg)

    def test_server_generic_project_limits_res_max(self):
        """
        Test max_queued_res for any project at the server level.
        """
        a = {"max_queued_res.ncpus":
             "[p:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        m = 'qsub: would exceed per-project limit on resource ncpus in complex'
        self.common_limit_test(True, a, attrs, exp_err=m)

    @skipOnShasta
    def test_server_group_limits_res_max(self):
        """
        Test max_queued_res for group pbsuser1 at the server level.
        """
        a = {"max_queued_res.ncpus":
             "[g:" + str(TSTGRP0) + "=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed group ' + str(TSTGRP0) + \
            "'s limit on resource ncpus in complex"
        self.common_limit_test(True, a, attrs, exp_err=errmsg)

    def test_server_overall_limits_res_max(self):
        """
        Test max_queued_res for all entities at the server level.
        """
        a = {"max_queued_res.ncpus":
             "[o:PBS_ALL=" + str(self.limit) + "]"}
        attrs = {'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed limit on resource ncpus in complex'
        self.common_limit_test(True, a, attrs, exp_err=errmsg)

    def test_queue_generic_user_limits_res_max(self):
        """
        Test max_queued_res for all users for the default queue.
        """
        a = {"max_queued_res.ncpus":
             "[u:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed per-user limit on resource ncpus in' \
            + ' queue ' + self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    def test_queue_user_limits_res_max(self):
        """
        Test max_queued_res for user pbsuser1 for the default queue.
        """
        a = {"max_queued_res.ncpus":
             "[u:" + str(TEST_USER) + "=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed user ' + str(TEST_USER) + \
            "'s limit on resource ncpus in queue " + \
            self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    @skipOnShasta
    def test_queue_group_limits_res_max(self):
        """
        Test max_queued_res for group pbsuser1 for the default queue
        """
        a = {"max_queued_res.ncpus":
             "[g:" + str(TSTGRP0) + "=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed group ' + str(TSTGRP0) + \
            "'s limit on resource ncpus in queue " + self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    @skipOnShasta
    def test_queue_generic_group_limits_res_max(self):
        """
        Test max_queued_res for any group for the default queue.
        """
        a = {"max_queued_res.ncpus":
             "[g:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed per-group limit on resource ncpus in' \
            + ' queue ' + self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    def test_queue_project_limits_res_max(self):
        """
        Test max_queued_res for project p1 for the default queue.
        """
        a = {"max_queued_res.ncpus":
             "[p:p1=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 ATTR_project: 'p1', 'Resource_List.select': '1:ncpus=1'}
        errmsg = "qsub: would exceed project p1's limit on resource ncpus" + \
            ' in queue ' + self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    def test_queue_generic_project_limits_res_max(self):
        """
        Test max_queued_res for any project for the default queue.
        """
        a = {"max_queued_res.ncpus":
             "[p:PBS_GENERIC=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed per-project limit on resource ncpus in' \
            + ' queue ' + self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    def test_queue_overall_limits_res_max(self):
        """
        Test max_queued_res for any entity for the default queue.
        """
        a = {"max_queued_res.ncpus":
             "[o:PBS_ALL=" + str(self.limit) + "]"}
        attrs = {ATTR_queue: self.server.default_queue,
                 'Resource_List.select': '1:ncpus=1'}
        errmsg = 'qsub: would exceed limit on resource ncpus in queue ' \
                 + self.server.default_queue
        self.common_limit_test(False, a, attrs, exp_err=errmsg)

    def test_qalter_resource(self):
        """
        Test that qaltering a job's resource list is accounted
        """
        res_name = 'res_long'
        res_attr = {ATTR_RESC_TYPE: 'long', ATTR_RESC_FLAG: 'q'}
        rc = self.server.manager(MGR_CMD_CREATE, RSC, res_attr, id=res_name)

        a = {"max_queued_res." + res_name:
             "[o:PBS_ALL=" + str(self.limit) + "]"}
        qname = self.server.default_queue
        self.server.manager(MGR_CMD_SET, QUEUE, a, qname)

        self.server.manager(MGR_CMD_SET, SERVER, {ATTR_scheduling: 'False'})

        attrs = {ATTR_queue: qname, 'Resource_List.' + res_name: 9}
        j_1 = Job(TEST_USER, attrs)
        J_1_id = self.server.submit(j_1)

        try:
            attrs = {ATTR_queue: qname, 'Resource_List.' + res_name: 2}
            j_2 = Job(TEST_USER, attrs)
            self.server.submit(j_2)
        except PbsSubmitError as e:
            exp_err = 'qsub: would exceed limit on resource ' + res_name + \
                ' in queue ' + qname
            if e.msg[0] != exp_err:
                raise self.failureException("rcvd unexpected err message: " +
                                            e.msg[0])

        attribs = {'Resource_List.' + res_name: 8}
        self.server.alterjob(J_1_id, attribs)
        self.server.expect(JOB, attribs, id=J_1_id)

        self.server.submit(j_2)

        try:
            attrs = {ATTR_queue: qname, 'Resource_List.' + res_name: 1}
            j_3 = Job(TEST_USER, attrs)
            self.server.submit(j_3)
        except PbsSubmitError as e:
            exp_err = 'qsub: would exceed limit on resource ' + res_name + \
                ' in queue ' + qname
            if e.msg[0] != exp_err:
                raise self.failureException("rcvd unexpected err message: " +
                                            e.msg[0])

    def test_multiple_queued_limits(self):
        defqname = self.server.default_queue
        q2name = 'q2'
        a = OrderedDict()
        a['queue_type'] = 'execution'
        a['enabled'] = 'True'
        a['started'] = 'True'
        self.server.manager(MGR_CMD_CREATE, QUEUE, a, id=q2name)

        a = {"queued_jobs_threshold":
             "[u:PBS_GENERIC=10]"}
        self.server.manager(MGR_CMD_SET, SERVER, a)
        a = {"queued_jobs_threshold":
             "[u:PBS_GENERIC=5]"}
        self.server.manager(MGR_CMD_SET, QUEUE, a, defqname)
        jd = Job(TEST_USER, {ATTR_queue: defqname})
        j2 = Job(TEST_USER, {ATTR_queue: q2name})

        jid = self.server.submit(jd)
        self.server.expect(JOB, {'job_state': 'R'}, id=jid)

        for _ in range(5):
            jd = Job(TEST_USER, {ATTR_queue: defqname})
            self.server.submit(jd)

        try:
            self.server.submit(jd)
        except PbsSubmitError as e:
            exp_err = "qsub: would exceed queue generic's per-user limit " + \
                "of jobs in 'Q' state"
            if e.msg[0] != exp_err:
                raise self.failureException("rcvd unexpected err message: " +
                                            e.msg[0])
        else:
            self.assertFalse(True, "Job violating limits got submitted.")

        for _ in range(5):
            self.server.submit(j2)

        try:
            self.server.submit(j2)
        except PbsSubmitError as e:
            exp_err = "qsub: would exceed complex's per-user limit of " + \
                "jobs in 'Q' state"
            if e.msg[0] != exp_err:
                raise self.failureException("rcvd unexpected err message: " +
                                            e.msg[0])
        else:
            self.assertFalse(True, "Job violating limits got submitted.")
