#! /usr/bin/env perl
##**************************************************************
##
## Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
## University of Wisconsin-Madison, WI.
## 
## Licensed under the Apache License, Version 2.0 (the "License"); you
## may not use this file except in compliance with the License.  You may
## obtain a copy of the License at
## 
##    http://www.apache.org/licenses/LICENSE-2.0
## 
## Unless required by applicable law or agreed to in writing, software
## distributed under the License is distributed on an "AS IS" BASIS,
## WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
## See the License for the specific language governing permissions and
## limitations under the License.
##
##**************************************************************

use CondorTest;
Condor::DebugOn();

$cmd = 'job_core_leaveinqueue-true_van.cmd';
$testname = 'Condor submit with test for policy trigger of leave_in_queue - vanilla U';

my $killedchosen = 0;

# truly const variables in perl
sub IDLE{1};
sub HELD{5};
sub COMPLETED{4};
sub RUNNING{2};

my %testerrors;
my %info;
my $cluster;

$executed = sub
{
	%info = @_;
	$cluster = $info{"cluster"};

	CondorTest::debug("Good. for leave_in_queue cluster $cluster must run first\n",1);
};

$success = sub
{
	my %info = @_;
	my $cluster = $info{"cluster"};
	my $return = 1;

	my $done = 0;
	my $retrycount = 0;
	my $cmd = "condor_q -format \"%s\" Owner -format \" ClusterId = %d\" ClusterId -format \" Status = %d\n\" JobStatus";
	while($done == 0) {
		my @adarray;
		my $status = 1;
		$status = CondorTest::runCondorTool($cmd,\@adarray,2);
		if(!$status) {
			CondorTest::debug("Test failure due to Condor Tool Failure<$cmd>\n",1);
			exit(1)
		}
		foreach my $line (@adarray) {
			CondorTest::debug("$line\n",1);
			if($line =~ /^\s*([\w\-\.]+)\s+ClusterId\s*=\s*$cluster\s*Status\s*=\s*(\d+)\s*.*$/) {
				CondorTest::debug("Following Line shows it is still in the queue...\n",1);
				CondorTest::debug("$line\n",1);
				if($2 != COMPLETED) {
					$retrycount = $retrycount +1;
					if($retrycount == 4) {
						CondorTest::debug("Can not find the cluster completed in the queue\n",1);
						last;
					} else {
						sleep((10 * $retrycount));
						next;
					}
				}
				CondorTest::debug("Found the cluster completed in the queue\n",1);
				$done = 1;
				$return = 0;
				last;
			}
		}
	}
	if($done == 0) {
		# we never found completed
		$return = 1;
	}

	CondorTest::debug("job should be done AND left in the queue!!\n",1);
	my @bdarray;
	my @cdarray;
	my $status = 1;
	my $cmd = "condor_rm $cluster";
	$status = CondorTest::runCondorTool($cmd,\@bdarray,2);
	if(!$status)
	{
		CondorTest::debug("Test failure due to Condor Tool Failure<$cmd>\n",1);
		exit(1)
	}
	$cmd = "condor_rm -forcex $cluster";
	$status = CondorTest::runCondorTool($cmd,\@cdarray,2);
	if(!$status)
	{
		CondorTest::debug("Test failure due to Condor Tool Failure<$cmd>\n",1);
		exit(1)
	}
	exit($return);
};

$submitted = sub
{
	my %info = @_;
	my $cluster = $info{"cluster"};

	CondorTest::debug("submitted: \n",1);
	{
		CondorTest::debug("good job $cluster expected submitted.\n",1);
	}
};

CondorTest::RegisterExecute($testname, $executed);
CondorTest::RegisterExitedSuccess( $testname, $success );
CondorTest::RegisterSubmit( $testname, $submitted );

if( CondorTest::RunTest($testname, $cmd, 0) ) {
	CondorTest::debug("$testname: SUCCESS\n",1);
	exit(0);
} else {
	die "$testname: CondorTest::RunTest() failed\n";
}

