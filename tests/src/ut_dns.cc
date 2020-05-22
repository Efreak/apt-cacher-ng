#include "gtest/gtest.h"
#include "dbman.h"
#include "csmapping.h"
#include "acfg.h"
#include "acngbase.h"
#include "sockio.h"
#include <dns/caddrinfo.h>

#include "gmock/gmock.h"

#include <unordered_map>

using namespace acng;

TEST(dns, remote)
{
	auto rc = CreateRegularSystemResources();
	CAddrInfoPtr dnsresult;
	rc->dnsbase->Resolve("localhost", "12345", [&](CAddrInfoPtr ainfo){
		dnsresult = ainfo;
		rc->fore->StartShutdown();
	});
	int status = rc->MainLoop();
	ASSERT_EQ(status, 0);
	ASSERT_TRUE(dnsresult);
	ASSERT_TRUE(dnsresult->getError().empty());
	auto result = formatIpPort(dnsresult->getTcpAddrInfo());
	//std::cout << result << std::endl;
	ASSERT_TRUE(result.length() > 10);
	ASSERT_EQ(result.substr(0, 3), "127");

}

