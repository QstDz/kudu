// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <sys/stat.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/client/client-test-util.h"
#include "kudu/client/client.h"
#include "kudu/client/client.pb.h"
#include "kudu/client/schema.h"
#include "kudu/client/shared_ptr.h"
#include "kudu/client/write_op.h"
#include "kudu/common/partial_row.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/master/master.pb.h"
#include "kudu/master/master.proxy.h"
#include "kudu/mini-cluster/external_mini_cluster.h"
#include "kudu/mini-cluster/mini_cluster.h"
#include "kudu/rpc/messenger.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/security/test/mini_kdc.h"
#include "kudu/security/token.pb.h"
#include "kudu/server/server_base.pb.h"
#include "kudu/server/server_base.proxy.h"
#include "kudu/tablet/key_value_test_schema.h"
#include "kudu/util/env.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/path_util.h"
#include "kudu/util/status.h"
#include "kudu/util/subprocess.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

DECLARE_string(local_ip_for_outbound_sockets);

using kudu::client::KuduClient;
using kudu::client::KuduInsert;
using kudu::client::KuduSchema;
using kudu::client::KuduSession;
using kudu::client::KuduTable;
using kudu::client::KuduTableCreator;
using kudu::cluster::ExternalMiniCluster;
using kudu::cluster::ExternalMiniClusterOptions;
using kudu::rpc::Messenger;
using std::string;
using std::unique_ptr;
using std::vector;
using strings::Substitute;

namespace kudu {

class SecurityITest : public KuduTest {
 public:
  SecurityITest() {
    cluster_opts_.enable_kerberos = true;
    cluster_opts_.num_tablet_servers = 3;
    cluster_opts_.extra_master_flags.emplace_back("--rpc_trace_negotiation");
    cluster_opts_.extra_tserver_flags.emplace_back("--rpc_trace_negotiation");
  }
  Status StartCluster() {
    cluster_.reset(new ExternalMiniCluster(cluster_opts_));
    return cluster_->Start();
  }

  Status TrySetFlagOnTS() {
    // Make a new messenger so that we don't reuse any cached connections from
    // the minicluster startup sequence.
    auto messenger = NewMessengerOrDie();
    const auto& addr = cluster_->tablet_server(0)->bound_rpc_addr();
    server::GenericServiceProxy proxy(messenger, addr, addr.host());

    rpc::RpcController controller;
    controller.set_timeout(MonoDelta::FromSeconds(30));
    server::SetFlagRequestPB req;
    server::SetFlagResponsePB resp;
    req.set_flag("non-existent");
    req.set_value("xx");
    return proxy.SetFlag(req, &resp, &controller);
  }

  // Create a table, insert a row, scan it back, and delete the table.
  void SmokeTestCluster();

  Status TryRegisterAsTS() {
    // Make a new messenger so that we don't reuse any cached connections from
    // the minicluster startup sequence.
    auto messenger = NewMessengerOrDie();
    const auto& addr = cluster_->master(0)->bound_rpc_addr();
    master::MasterServiceProxy proxy(messenger, addr, addr.host());

    rpc::RpcController rpc;
    master::TSHeartbeatRequestPB req;
    master::TSHeartbeatResponsePB resp;
    req.mutable_common()->mutable_ts_instance()->set_permanent_uuid("x");
    req.mutable_common()->mutable_ts_instance()->set_instance_seqno(1);
    return proxy.TSHeartbeat(req, &resp, &rpc);
  }

 private:
  std::shared_ptr<Messenger> NewMessengerOrDie() {
    std::shared_ptr<Messenger> messenger;
    CHECK_OK(rpc::MessengerBuilder("test-messenger")
             .set_num_reactors(1)
             .set_max_negotiation_threads(1)
             .Build(&messenger));
    return messenger;
  }

 protected:
  ExternalMiniClusterOptions cluster_opts_;
  unique_ptr<ExternalMiniCluster> cluster_;
};

void SecurityITest::SmokeTestCluster() {
  const char* kTableName = "test-table";
  client::sp::shared_ptr<KuduClient> client;
  ASSERT_OK(cluster_->CreateClient(nullptr, &client));

  // Create a table.
  KuduSchema schema = client::KuduSchemaFromSchema(CreateKeyValueTestSchema());
  gscoped_ptr<KuduTableCreator> table_creator(client->NewTableCreator());
  ASSERT_OK(table_creator->table_name(kTableName)
            .set_range_partition_columns({ "key" })
            .schema(&schema)
            .num_replicas(3)
            .Create());

  // Insert a row.
  client::sp::shared_ptr<KuduTable> table;
  ASSERT_OK(client->OpenTable(kTableName, &table));
  client::sp::shared_ptr<KuduSession> session = client->NewSession();
  session->SetTimeoutMillis(60000);
  unique_ptr<KuduInsert> ins(table->NewInsert());
  ASSERT_OK(ins->mutable_row()->SetInt32(0, 12345));
  ASSERT_OK(ins->mutable_row()->SetInt32(1, 54321));
  ASSERT_OK(session->Apply(ins.release()));
  FlushSessionOrDie(session);

  // Read it back.
  ASSERT_EQ(1, CountTableRows(table.get()));

  // Delete the table.
  ASSERT_OK(client->DeleteTable(kTableName));
}

// Test creating a table, writing some data, reading data, and dropping
// the table.
TEST_F(SecurityITest, SmokeTestAsAuthorizedUser) {
  ASSERT_OK(StartCluster());

  ASSERT_OK(cluster_->kdc()->Kinit("test-user"));
  NO_FATALS(SmokeTestCluster());

  // Non-superuser clients should not be able to set flags.
  Status s = TrySetFlagOnTS();
  ASSERT_EQ("Remote error: Not authorized: unauthorized access to method: SetFlag",
            s.ToString());

  // Nor should they be able to send TS RPCs.
  s = TryRegisterAsTS();
  ASSERT_EQ("Remote error: Not authorized: unauthorized access to method: TSHeartbeat",
            s.ToString());
}

// Test trying to access the cluster with no Kerberos credentials at all.
TEST_F(SecurityITest, TestNoKerberosCredentials) {
  ASSERT_OK(StartCluster());
  ASSERT_OK(cluster_->kdc()->Kdestroy());

  client::sp::shared_ptr<KuduClient> client;
  Status s = cluster_->CreateClient(nullptr, &client);
  ASSERT_STR_MATCHES(s.ToString(),
                     "Not authorized: Could not connect to the cluster: "
                     "Client connection negotiation failed: client connection "
                     "to .*: server requires authentication, "
                     "but client does not have Kerberos credentials available");
}

// Regression test for KUDU-2121. Set up a Kerberized cluster with optional
// authentication. An un-Kerberized client should be able to connect with SASL
// PLAIN authentication.
TEST_F(SecurityITest, SaslPlainFallback) {
  cluster_opts_.num_masters = 1;
  cluster_opts_.num_tablet_servers = 0;
  cluster_opts_.extra_master_flags.emplace_back("--rpc-authentication=optional");
  cluster_opts_.extra_master_flags.emplace_back("--user-acl=*");
  ASSERT_OK(StartCluster());
  ASSERT_OK(cluster_->kdc()->Kdestroy());

  client::sp::shared_ptr<KuduClient> client;
  ASSERT_OK(cluster_->CreateClient(nullptr, &client));

  // Check client can successfully call ListTables().
  vector<string> tables;
  ASSERT_OK(client->ListTables(&tables));
}

// Test cluster access by a user who is not authorized as a client.
TEST_F(SecurityITest, TestUnauthorizedClientKerberosCredentials) {
  ASSERT_OK(StartCluster());
  ASSERT_OK(cluster_->kdc()->Kinit("joe-interloper"));
  client::sp::shared_ptr<KuduClient> client;
  Status s = cluster_->CreateClient(nullptr, &client);
  ASSERT_EQ("Remote error: Could not connect to the cluster: "
            "Not authorized: unauthorized access to method: ConnectToMaster",
            s.ToString());
}

// Test superuser actions when authorized as a superuser.
TEST_F(SecurityITest, TestAuthorizedSuperuser) {
  ASSERT_OK(StartCluster());

  ASSERT_OK(cluster_->kdc()->Kinit("test-admin"));

  // Superuser can set flags.
  ASSERT_OK(TrySetFlagOnTS());

  // Even superusers can't pretend to be tablet servers.
  Status s = TryRegisterAsTS();

  ASSERT_EQ("Remote error: Not authorized: unauthorized access to method: TSHeartbeat",
            s.ToString());

}

// Test that the web UIs can be entirely disabled, for users who feel they
// are a security risk.
TEST_F(SecurityITest, TestDisableWebUI) {
  cluster_opts_.extra_master_flags.emplace_back("--webserver_enabled=0");
  cluster_opts_.extra_tserver_flags.emplace_back("--webserver_enabled=0");
  ASSERT_OK(StartCluster());
  NO_FATALS(SmokeTestCluster());
}

// Test disabling authentication and encryption.
TEST_F(SecurityITest, TestDisableAuthenticationEncryption) {
  cluster_opts_.extra_master_flags.emplace_back("--rpc_authentication=disabled");
  cluster_opts_.extra_tserver_flags.emplace_back("--rpc_authentication=disabled");
  cluster_opts_.extra_master_flags.emplace_back("--rpc_encryption=disabled");
  cluster_opts_.extra_tserver_flags.emplace_back("--rpc_encryption=disabled");
  cluster_opts_.enable_kerberos = false;
  ASSERT_OK(StartCluster());
  NO_FATALS(SmokeTestCluster());
}

void CreateWorldReadableFile(const string& name) {
  unique_ptr<RWFile> file;
  ASSERT_OK(Env::Default()->NewRWFile(name, &file));
  ASSERT_EQ(chmod(name.c_str(), 0444), 0);
}

void GetFullBinaryPath(string* binary) {
  string exe;
  ASSERT_OK(Env::Default()->GetExecutablePath(&exe));
  (*binary) = JoinPathSegments(DirName(exe), *binary);
}

TEST_F(SecurityITest, TestWorldReadableKeytab) {
  const string credentials_name = GetTestPath("insecure.keytab");
  NO_FATALS(CreateWorldReadableFile(credentials_name));
  string binary = "kudu-master";
  NO_FATALS(GetFullBinaryPath(&binary));
  const vector<string> argv = { binary, Substitute("--keytab_file=$0", credentials_name) };
  string stderr;
  Status s = Subprocess::Call(argv, "", nullptr, &stderr);
  ASSERT_STR_CONTAINS(stderr, Substitute(
      "cannot use keytab file with world-readable permissions: $0",
      credentials_name));
}

TEST_F(SecurityITest, TestWorldReadablePrivateKey) {
  const string credentials_name = GetTestPath("insecure.key");
  NO_FATALS(CreateWorldReadableFile(credentials_name));
  string binary = "kudu-master";
  NO_FATALS(GetFullBinaryPath(&binary));
  const vector<string> argv = { binary,
                                "--unlock_experimental_flags",
                                Substitute("--rpc_private_key_file=$0", credentials_name),
                                "--rpc_certificate_file=fake_file",
                                "--rpc_ca_certificate_file=fake_file" };
  string stderr;
  Status s = Subprocess::Call(argv, "", nullptr, &stderr);
  ASSERT_STR_CONTAINS(stderr, Substitute(
      "cannot use private key file with world-readable permissions: $0",
      credentials_name));
}

struct AuthTokenIssuingTestParams {
  const ExternalMiniCluster::BindMode bind_mode;
  const string rpc_authentication;
  const string rpc_encryption;
  const bool rpc_encrypt_loopback_connections;
  const bool authn_token_present;
};
class AuthTokenIssuingTest :
    public SecurityITest,
    public ::testing::WithParamInterface<AuthTokenIssuingTestParams> {
};
INSTANTIATE_TEST_CASE_P(, AuthTokenIssuingTest, ::testing::ValuesIn(
    vector<AuthTokenIssuingTestParams>{
      { ExternalMiniCluster::BindMode::LOOPBACK, "required", "required", true,  true,  },
      { ExternalMiniCluster::BindMode::LOOPBACK, "required", "required", false, true,  },
      //ExternalMiniCluster::BindMode::LOOPBACK, "required", "disabled": non-acceptable
      //ExternalMiniCluster::BindMode::LOOPBACK, "required", "disabled": non-acceptable
      { ExternalMiniCluster::BindMode::LOOPBACK, "disabled", "required", true,  true,  },
      { ExternalMiniCluster::BindMode::LOOPBACK, "disabled", "required", false, true,  },
      { ExternalMiniCluster::BindMode::LOOPBACK, "disabled", "disabled", true,  false, },
      { ExternalMiniCluster::BindMode::LOOPBACK, "disabled", "disabled", false, true,  },
#if defined(__linux__)
      { ExternalMiniCluster::BindMode::UNIQUE_LOOPBACK, "required", "required", true,  true,  },
      { ExternalMiniCluster::BindMode::UNIQUE_LOOPBACK, "required", "required", false, true,  },
      //ExternalMiniCluster::BindMode::UNIQUE_LOOPBACK, "required", "disabled": non-acceptable
      //ExternalMiniCluster::BindMode::UNIQUE_LOOPBACK, "required", "disabled": non-acceptable
      { ExternalMiniCluster::BindMode::UNIQUE_LOOPBACK, "disabled", "required", true,  true,  },
      { ExternalMiniCluster::BindMode::UNIQUE_LOOPBACK, "disabled", "required", false, true,  },
      { ExternalMiniCluster::BindMode::UNIQUE_LOOPBACK, "disabled", "disabled", true,  false, },
      { ExternalMiniCluster::BindMode::UNIQUE_LOOPBACK, "disabled", "disabled", false, false, },
#endif
    }
));

// Verify how master issues authn tokens to clients. Master sends authn tokens
// to clients upon call of the ConnectToMaster() RPC. The master's behavior
// must depend on whether the connection to the client is confidential or not.
TEST_P(AuthTokenIssuingTest, ChannelConfidentiality) {
  cluster_opts_.num_masters = 1;
  cluster_opts_.num_tablet_servers = 0;
  // --user-acl: just restoring back the default setting.
  cluster_opts_.extra_master_flags.emplace_back("--user-acl=*");

  const auto& params = GetParam();
  cluster_opts_.bind_mode = params.bind_mode;
  cluster_opts_.extra_master_flags.emplace_back(
      Substitute("--rpc-authentication=$0", params.rpc_authentication));
  cluster_opts_.extra_master_flags.emplace_back(
      Substitute("--rpc-encryption=$0", params.rpc_encryption));
  cluster_opts_.extra_master_flags.emplace_back(
      Substitute("--rpc_encrypt_loopback_connections=$0",
                 params.rpc_encrypt_loopback_connections));
  ASSERT_OK(StartCluster());

  // Make sure the client always connects from the standard loopback address.
  // This is crucial when the master is running with UNIQUE_LOOPBACK mode: the
  // test scenario expects the client connects from other than 127.0.0.1 address
  // so the connection is not considered a 'loopback' one.
  FLAGS_local_ip_for_outbound_sockets = "127.0.0.1";

  // Current implementation of MasterServiceImpl::ConnectToMaster() allows to
  // get a success response without proper security information in case if the
  // master hasn't been established as a leader yet. As a temporary workaround,
  // make sure the master is sending back the necessary info before going any
  // further with the scenario which is sensitive to that issue.
  //
  // TODO(aserbin): fix the issue with MasterServiceImpl::ConnectToMaster()
  //                and remove this ASSERT_EVENTUALLY() block.
  ASSERT_EVENTUALLY([&] {
    client::sp::shared_ptr<KuduClient> client;
    ASSERT_OK(cluster_->CreateClient(nullptr, &client));

    string authn_creds;
    ASSERT_OK(client->ExportAuthenticationCredentials(&authn_creds));
    client::AuthenticationCredentialsPB pb;
    ASSERT_TRUE(pb.ParseFromString(authn_creds));
    ASSERT_GE(pb.ca_cert_ders_size(), 1);
  });

  // In current implementation, KuduClientBuilder calls ConnectToCluster() on
  // the newly created instance of the KuduClient.
  client::sp::shared_ptr<KuduClient> client;
  ASSERT_OK(cluster_->CreateClient(nullptr, &client));

  string authn_creds;
  ASSERT_OK(client->ExportAuthenticationCredentials(&authn_creds));
  client::AuthenticationCredentialsPB pb;
  ASSERT_TRUE(pb.ParseFromString(authn_creds));
  ASSERT_EQ(params.authn_token_present, pb.has_authn_token());

  if (pb.has_authn_token()) {
    // If authn token is present, then check it for consistency.
    const auto& t = pb.authn_token();
    EXPECT_TRUE(t.has_token_data());
    EXPECT_TRUE(t.has_signature());
    EXPECT_TRUE(t.has_signing_key_seq_num());
  }
}

} // namespace kudu
