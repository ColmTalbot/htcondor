/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


#include "condor_common.h"
#include "condor_config.h"
#include "condor_attributes.h"
#include "condor_state.h"
#include "status_types.h"
#include "get_daemon_name.h"
#include "sig_install.h"
#include "daemon.h"
#include "dc_collector.h"
#include "extArray.h"
#include "string_list.h"
#include "MyString.h"
#include "match_prefix.h"    // is_arg_colon_prefix
#include "condor_api.h"
#include "condor_string.h"   // for strnewp()
#include "status_types.h"
#include "directory.h"
#include <my_popen.h>

#include "condor_distribution.h"
#include "condor_version.h"

#include <vector>
#include <sstream>
#include <map>
#include <iostream>

using std::vector;
using std::map;
using std::string;
using std::stringstream;

// stuff that we can scrape from the log directory about various daemons.
//
typedef struct {
	std::string name;  // Daemon name, (note Starters are named SlotN)
	std::string addr;  // sinful string from .address file, if available
	std::string log_dir;// log directory
	std::string log;     // log filename
	std::string log_old; // previous (.old) log file full path name.
	std::string exe_path; // full path to executable (from master log)
	std::string pid;      // latest process id (from master log, starterlog, etc)
} LOG_INFO;

typedef std::map<std::string, LOG_INFO*> LOG_INFO_MAP;
typedef std::map<std::string, unsigned int> MAP_TO_PID;
typedef	std::map<unsigned int, std::string> MAP_FROM_PID;

//char	*param();
static void query_log_dir(const char * log_dir, LOG_INFO_MAP & info);
static void print_log_info(LOG_INFO_MAP & info);
static void scan_logs_for_info(LOG_INFO_MAP & info, MAP_TO_PID & job_to_pid);
static void query_daemons_for_pids(LOG_INFO_MAP & info);
static void ping_all_known_addrs(LOG_INFO_MAP & info);
static char * get_daemon_param(std::string addr, char * param_name);

// app globals
static struct {
	const char * Name; // tool name as invoked from argv[0]
	List<const char> target_names;    // list of target names to query
	List<LOG_INFO>   all_log_info;    // pool of info from scanning log directories.

	List<const char> print_head; // The list of headings for the mask entries
	AttrListPrintMask print_mask;
	ArgList projection;    // Attributes that we want the server to send us

	bool   diagnostic; // output useful only to developers
	bool   verbose;    // extra output useful to users
	bool   full_ads;
	bool   job_ad;		     // debugging
	bool   ping_all_addrs;	 // 
	vector<int> query_pids;
	vector<const char *> query_log_dirs;
	vector<const char *> query_addrs;
	vector<const char *> constraint;

	// hold temporary results.
	MAP_TO_PID   job_to_pid;
	MAP_FROM_PID pid_to_program;

} App;

// init fields in the App structure that have no default initializers
void InitAppGlobals(const char * argv0)
{
	App.Name = argv0;
	App.diagnostic = false; // output useful only to developers
	App.verbose = false;    // extra output useful to users
	App.full_ads = false;
	App.job_ad = false;     // debugging
	App.ping_all_addrs = false;
}

	// Tell folks how to use this program
void usage(bool and_exit)
{
	fprintf (stderr, 
		"Usage: %s [help-opt] [addr-opt] [query-opt] [display-opt]\n"
		"    where [help-opt] is one of\n"
		"\t-h[elp]\t\t\tThis screen\n"
		"\t-diag[nostic]\t\tPrint out query ad without performing query\n"
		"    and [addr-opt] is one of\n"
		"\t-addr[ess] <host>\t\tSCHEDD host address to query\n"
		"\t-log[dir] <dir>\t\tDirectory to seach for SCHEDD host address to query\n"
		"\t-pid <pid>\t\tSCHEDD Process to query\n"
		"   and [display-opt] is one or more of\n"
		"\t-ps\t\t\tDisplay process tree\n"
		"\t-l[ong]\t\t\tDisplay entire classads\n"
		"\t-v[erbose]\t\tSame as -long\n"
		"\t-f[ormat] <fmt> <attr>\tPrint attribute with a format specifier\n"
		"\t-a[uto]f[ormat]:[V,ntlh] <attr1> [attr2 [attr3 ...]]\tPrint attr(s) with automatic formatting\n"
		"\t\t,\tUse %%V formatting\n"
		"\t\t,\tComma separated (default is space separated)\n"
		"\t\tt\tTab separated\n"
		"\t\tn\tNewline after each attribute\n"
		"\t\tl\tLabel each value\n"
		"\t\th\tHeadings\n"
		, App.Name);
	if (and_exit)
		exit( 1 );
}

void AddPrintColumn(const char * heading, int width, const char * expr)
{
	ClassAd ad;
	StringList attributes;
	if(!ad.GetExprReferences(expr, attributes, attributes)) {
		fprintf( stderr, "Error:  Parse error of: %s\n", expr);
		exit(1);
	}

	attributes.rewind();
	while (const char *str = attributes.next()) {
		App.projection.AppendArg(str);
	}

	App.print_head.Append(heading);

	int wid = width ? width : strlen(heading);
	int opts = FormatOptionNoTruncate | FormatOptionAutoWidth;
	App.print_mask.registerFormat("%v", wid, opts, expr);
}

static const char *
format_int_runtime (int utime, AttrList * /*ad*/, Formatter & /*fmt*/)
{
	return format_time(utime);
}

static const char *
format_jobid_pid (char *jobid, AttrList * /*ad*/, Formatter & /*fmt*/)
{
	static char outstr[100];
	outstr[0] = 0;
	if (App.job_to_pid.find(jobid) != App.job_to_pid.end()) {
		sprintf(outstr, "%u", App.job_to_pid[jobid]);
	}
	return outstr;
}

// parse a file stream and return the first field value from the column named fld_name.
// 
// stream is expected to be the output of a tool like ps or tasklist that returns
// data in tabular form with a header (parse_type==0),  or one labeled value per line
// form like a long form classad (parse_type=1)
int get_field_from_stream(FILE * stream, int parse_type, const char * fld_name, std::string & outstr)
{
	int cch = 0;
	outstr.clear();

	if (0 == parse_type) {
		// stream contains
		// HEADINGS over DATA with optional ==== or --- line under headings.
		char * line = getline(stream);
		if (line && ! strlen(line)) line = getline(stream);
		if (line) {
			std::string headings = line;
			std::string subhead;
			std::string data;

			line = getline(stream);
			if (line) data = line;

			if (data.find("====") == 0 || data.find("----") == 0) {
				subhead = line;
				data.clear();

				// first line after headings is not data, but underline
				line = getline(stream);
				if (line) data = line;
			}

			const char WS[] = " \t\r\n";
			if (subhead.empty()) {
				// use field index, assume all but last field have no spaces in them.
				unsigned int ixh=0, ixd=0, ixh2, ixd2;
				for (;;) {
					if (ixh == string::npos || ixd == string::npos)
						break;
					ixh2 = ixh = headings.find_first_not_of(WS, ixh);
					if (ixh2 != string::npos) ixh2 = headings.find_first_of(WS, ixh2);
					ixd2 = ixd = data.find_first_not_of(WS, ixd);
					if (ixd2 != string::npos) ixd2 = data.find_first_of(WS, ixd);
					std::string hd;
					if (ixh2 != string::npos)
						hd = headings.substr(ixh, ixh2-ixh);
					else
						hd = headings.substr(ixh);

					if (hd == fld_name) {
						ixd = data.find_first_not_of(WS, ixd2);
						bool to_eol = headings.find_first_not_of(WS, ixh2) != string::npos;
						if (to_eol || ixd == string::npos) {
							outstr = data.substr(ixd2);
						} else {
							outstr = data.substr(ixd, ixd2-ixd);
						}
						cch = outstr.size();
						break;
					}
					ixh = ixh2;
					ixd = ixd2;
				}
			} else {
				// use subhead to get field widths
				unsigned int ixh = 0, ixh2;
				for (;;) {
					if (ixh == string::npos)
						break;

					std::string hd;
					ixh2 = ixh = subhead.find_first_not_of(WS, ixh);
					if (ixh2 != string::npos) 
						ixh2 = subhead.find_first_of(WS, ixh2);
					if (ixh2 != string::npos)
						hd = headings.substr(ixh, ixh2-ixh);
					else
						hd = headings.substr(ixh);

					trim(hd);
					if (hd == fld_name) {
						if (ixh2 != string::npos)
							outstr = data.substr(ixh, ixh2-ixh);
						else
							outstr = data.substr(ixh);
						cch = outstr.size();
						break;
					}

					ixh = ixh2;
				}
			}
		}
	} else if (1 == parse_type) { 
		// stream contains lines with
		// Label: value
		//  or 
		// Label = value

	}

	return cch;
}

#ifdef WIN32
#include <psapi.h>
#endif

static void init_program_for_pid(unsigned int pid)
{
	App.pid_to_program[pid] = "";

#if 0
	#ifdef WIN32
		extern DWORD WINAPI GetProcessImageFileNameA(HANDLE, LPSTR, DWORD);
		HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
		if (hProcess) {
			char outstr[MAX_PATH];
			if (GetProcessImageFileNameA(hProcess, outstr, sizeof(outstr)))
				App.pid_to_program[pid] = outstr;
			CloseHandle(hProcess);
		}
	#endif
#else
		std::string cmdline;
	#ifdef WIN32
		//const char * cmd_format = "tasklist /FO LIST /FI \"PID eq %u\" ";
		const char * cmd_format = "tasklist /FI \"PID eq %u\" ";
		const char * fld_name = "Image Name";
		//const int    parse_type = 1;
		const int    parse_type = 0;
	#else
		const char * cmd_format = "ps -f %u";
		const char * fld_name = "CMD";
		const  int    parse_type = 0;
	#endif
		sprintf(cmdline, cmd_format, pid);
		const char * pcmdline = cmdline.c_str();
		FILE * stream = my_popen(pcmdline, "r", FALSE);
		if (stream) {
			std::string program;
			if (get_field_from_stream(stream, parse_type, fld_name, program))
				App.pid_to_program[pid] = program;
			my_pclose(stream);
		}
#endif
}

static const char *
format_jobid_program (char *jobid, AttrList * /*ad*/, Formatter & /*fmt*/)
{
	const char * outstr = NULL;

	if (App.job_to_pid.find(jobid) != App.job_to_pid.end()) {
		unsigned int pid = App.job_to_pid[jobid];
		if (App.pid_to_program.find(pid) == App.pid_to_program.end()) {
			init_program_for_pid(pid);
		}
		outstr = App.pid_to_program[pid].c_str();
	}

	return outstr;
}

void AddPrintColumn(const char * heading, int width, const char * attr, StringCustomFmt fmt)
{
	App.projection.AppendArg(attr);
	App.print_head.Append(heading);

	int wid = width ? width : strlen(heading);
	int opts = FormatOptionNoTruncate | FormatOptionAutoWidth;
	App.print_mask.registerFormat(NULL, wid, opts, fmt, attr);
}

void AddPrintColumn(const char * heading, int width, const char * attr, IntCustomFmt fmt)
{
	App.projection.AppendArg(attr);
	App.print_head.Append(heading);

	int wid = width ? width : strlen(heading);
	int opts = FormatOptionNoTruncate | FormatOptionAutoWidth;
	App.print_mask.registerFormat(NULL, wid, opts, fmt, attr);
}

#define IsArg is_arg_prefix

void parse_args(int /*argc*/, char *argv[])
{
	for (int ixArg = 0; argv[ixArg]; ++ixArg)
	{
		const char * parg = argv[ixArg];
		if (is_dash_arg_prefix(parg, "help", 1)) {
			usage(true);
			continue;
		}
		if (is_dash_arg_prefix(parg, "debug", 1)) {
			const char * pflags = argv[ixArg+1];
			if (pflags && (pflags[0] == '"' || pflags[0] == '\'')) ++pflags;
			if (pflags && pflags[0] == 'D' && pflags[1] == '_') {
				++ixArg;
			} else {
				pflags = NULL;
			}
			Termlog = 1;
			dprintf_config ("TOOL", get_param_functions());
			set_debug_flags( pflags, D_NOHEADER | D_FULLDEBUG );
			continue;
		}

		if (*parg != '-') {
			// arg without leaging - is a target
			App.target_names.Append(parg);
		} else {
			// arg with leading '-' is an option
			const char * pcolon = NULL;
			++parg;
			if (IsArg(parg, "diagnostic", 4)) {
				App.diagnostic = true;
			} else if (IsArg(parg, "verbose", 4)) {
				App.verbose = true;
			} else if (IsArg(parg, "address", 4)) {
				if ( ! argv[ixArg+1] || *argv[ixArg+1] == '-') {
					fprintf(stderr, "Error: Argument %s requires a host address\n", argv[ixArg]);
					exit(1);
				}
				++ixArg;
				App.query_addrs.push_back(argv[ixArg]);
			} else if (IsArg(parg, "logdir", 3)) {
				if ( ! argv[ixArg+1] || *argv[ixArg+1] == '-') {
					fprintf(stderr, "Error: Argument %s requires a directory\n", argv[ixArg]);
					exit(1);
				}
				++ixArg;
				App.query_log_dirs.push_back(argv[ixArg]);
			} else if (IsArg(parg, "pid", 3)) {
				if ( ! argv[ixArg+1] || *argv[ixArg+1] == '-') {
					fprintf(stderr, "Error: Argument %s requires a process id parameter\n", argv[ixArg]);
					exit(1);
				}
				++ixArg;
				char * p;
				int pid = strtol(argv[ixArg+1], &p, 10);
				App.query_pids.push_back(pid);
			} else if (IsArg(parg, "ps", 2)) {
			} else if (IsArg(parg, "long", 1)) {
				App.full_ads = true;
			} else if (IsArg(parg, "format", 1)) {
			} else if (is_arg_colon_prefix(parg, "autoformat", &pcolon, 5) ||
			           is_arg_colon_prefix(parg, "af", &pcolon, 2)) {
				// make sure we have at least one more argument
				if ( !argv[ixArg+1] || *(argv[ixArg+1]) == '-') {
					fprintf (stderr, "Error: Argument %s requires "
					         "at last one attribute parameter\n", argv[ixArg]);
					fprintf (stderr, "Use \"%s -help\" for details\n", App.Name);
					exit(1);
				}


				bool flabel = false;
				bool fCapV  = false;
				bool fheadings = false;
				const char * pcolpre = " ";
				const char * pcolsux = NULL;
				if (pcolon) {
					++pcolon;
					while (*pcolon) {
						switch (*pcolon)
						{
							case ',': pcolsux = ","; break;
							case 'n': pcolsux = "\n"; break;
							case 't': pcolpre = "\t"; break;
							case 'l': flabel = true; break;
							case 'V': fCapV = true; break;
							case 'h': fheadings = true; break;
						}
						++pcolon;
					}
				}
				App.print_mask.SetAutoSep(NULL, pcolpre, pcolsux, "\n");

				while (argv[ixArg+1] && *(argv[ixArg+1]) != '-') {
					++ixArg;
					ClassAd ad;
					StringList attributes;
					if(!ad.GetExprReferences(argv[ixArg], attributes, attributes)) {
						fprintf( stderr, "Error:  Parse error of: %s\n", argv[ixArg]);
						exit(1);
					}

					attributes.rewind();
					while (const char *str = attributes.next()) {
						App.projection.AppendArg(str);
					}

					MyString lbl = "";
					int wid = 0;
					int opts = FormatOptionNoTruncate;
					if (fheadings || App.print_head.Length() > 0) { 
						const char * hd = fheadings ? argv[ixArg] : "(expr)";
						wid = 0 - (int)strlen(hd); 
						opts = FormatOptionAutoWidth | FormatOptionNoTruncate; 
						App.print_head.Append(hd);
					}
					else if (flabel) { lbl.sprintf("%s = ", argv[ixArg]); wid = 0; opts = 0; }

					lbl += fCapV ? "%V" : "%v";
					if (App.diagnostic) {
						printf ("Arg %d --- register format [%s] width=%d, opt=0x%x for [%s]\n",
								ixArg, lbl.Value(), wid, opts,  argv[ixArg]);
					}
					App.print_mask.registerFormat(lbl.Value(), wid, opts, argv[ixArg]);
				}
			} else if (IsArg(parg, "job", 3)) {
				App.job_ad = true;
			} else if (IsArg(parg, "ping_all_addrs", 4)) {
				App.ping_all_addrs = true;
			}
		}
	}

	// done parsing args, now interpret the results.
	//

	// if no output format has yet been specified, choose a default.
	//
	if ( ! App.full_ads && App.print_mask.IsEmpty() ) {
		App.print_mask.SetAutoSep(NULL, " ", NULL, "\n");

		if (App.job_ad) {
			AddPrintColumn("USER", 0, "User");
			AddPrintColumn("CMD", 0, "Cmd");
			AddPrintColumn("MEMORY", 0, "MemoryUsage");
		} else {

			//SLOT OWNER   PID   RUNTIME  MEMORY  COMMAND
			AddPrintColumn("OWNER", 0, "RemoteOwner");
			AddPrintColumn("SLOT", 0, "SlotId"); 
			AddPrintColumn("JOB", -6, "JobId"); 
			AddPrintColumn("  RUNTIME", 12, "TotalJobRunTime", format_int_runtime);
			AddPrintColumn("PID", -6, "JobId", format_jobid_pid); 
			AddPrintColumn("PROGRAM", 0, "JobId", format_jobid_program);
		}
	}

	if (App.job_ad) {
		// constraint?
	} else {
		App.constraint.push_back("JobId=!=UNDEFINED");
	}
}

int
main( int argc, char *argv[] )
{
	InitAppGlobals(argv[0]);

#if !defined(WIN32)
	install_sig_handler(SIGPIPE, SIG_IGN );
#endif

	myDistro->Init( argc, argv );
	config();

	if( argc < 1 ) {
		usage(true);
	}

	// parse command line arguments here.
	parse_args(argc, argv);

	if (App.query_log_dirs.size() > 0) {
		if (App.diagnostic) {
			printf("Query log dirs:\n");
			for (unsigned int ii = 0; ii < App.query_log_dirs.size(); ++ii) {
				printf("    [%3d] %s\n", ii, App.query_log_dirs[ii]);
			}
		}
		for (unsigned int ii = 0; ii < App.query_log_dirs.size(); ++ii) {
			LOG_INFO_MAP info;
			query_log_dir(App.query_log_dirs[ii], info);
			scan_logs_for_info(info, App.job_to_pid);

			// if we got a STARTD address, push it's address onto the list 
			// that we want to query.
			LOG_INFO_MAP::const_iterator it = info.find("Startd");
			if (it != info.end()) {
				LOG_INFO * pli = it->second;
				if ( ! pli->addr.empty()) {
					App.query_addrs.push_back(strdup(pli->addr.c_str()));
				}
			}

			if(App.ping_all_addrs) { ping_all_known_addrs(info); }
			query_daemons_for_pids(info);

			if (App.diagnostic | App.verbose) {
				printf("\nLOG directory \"%s\"\n", App.query_log_dirs[ii]);
				print_log_info(info);
			}
		}
	}

	CondorQuery *query;
	if (App.job_ad)
		query = new CondorQuery(ANY_AD);
	else
		query = new CondorQuery(STARTD_AD);

	if ( ! query) {
		fprintf (stderr, "Error:  Out of memory\n");
		exit (1);
	}

	if (App.constraint.size() > 0) {
		for (unsigned int ii = 0; ii < App.constraint.size(); ++ii) {
			query->addANDConstraint (App.constraint[ii]);
		}
	}

	if (App.projection.Count() > 0) {
		char **attr_list = App.projection.GetStringArray();
		query->setDesiredAttrs(attr_list);
		deleteStringArray(attr_list);
	}

	// if diagnose was requested, just print the query ad
	if (App.diagnostic) {

		// print diagnostic information about inferred internal state
		printf ("----------\n");

		ClassAd queryAd;
		QueryResult qr = query->getQueryAd (queryAd);
		queryAd.fPrint (stdout);

		printf ("----------\n");
		fprintf (stderr, "Result of making query ad was:  %d\n", qr);
		//exit (1);
	}

	// if we have accumulated no query addresses, then query the default
	if (App.query_addrs.empty()) {
		App.query_addrs.push_back("NULL");
	}

	for (unsigned int ixAddr = 0; ixAddr < App.query_addrs.size(); ++ixAddr) {

		const char * direct = NULL; //"<128.105.136.32:7977>";
		const char * addr = App.query_addrs[ixAddr];

		if (App.diagnostic) { printf("querying addr = %s\n", addr); }

		if (strcasecmp(addr,"NULL") == MATCH) {
			addr = NULL;
		} else {
			direct = addr;
		}


		Daemon *dae = new Daemon( DT_STARTD, direct, addr );
		if ( ! dae) {
			fprintf (stderr, "Error:  Could not create Daemon object for %s\n", direct);
			exit (1);
		}

		if( dae->locate() ) {
			addr = dae->addr();
		}
		if (App.diagnostic) { printf("got a Daemon, addr = %s\n", addr); }

		ClassAdList result;
		if (addr || App.diagnostic) {
			CondorError errstack;
			QueryResult qr = query->fetchAds (result, addr, &errstack);
			if (Q_OK != qr) {
				fprintf( stderr, "Error: %s\n", getStrQueryResult(qr) );
				fprintf( stderr, "%s\n", errstack.getFullText(true) );
				exit(1);
			}
			else if (App.diagnostic) {
				printf("QueryResult is %d : %s\n", qr, errstack.getFullText(true));
				printf("    %d records\n", result.Length());
			}
		}


		// extern int mySortFunc(AttrList*,AttrList*,void*);
		// result.Sort((SortFunctionType)mySortFunc);

		// prettyPrint (result, &totals);
		bool fFirstLine = true;
		result.Open();
		while (ClassAd	*ad = result.Next()) {
			if (App.diagnostic) {
				printf("    result Ad has %d attributes\n", ad->size());
			}
			if (App.full_ads) {
				ad->fPrint(stdout);
				printf("\n");
			} else {
				if (fFirstLine) {
					// print the first line so that we get column widths.
					char * tmp = App.print_mask.display(ad);
					delete [] tmp;
					// now print headings
					printf("\n");
					App.print_mask.display_Headings(stdout, App.print_head);
					//printf("\n");
					fFirstLine = false;
				}
				// now print rows of data
				App.print_mask.display (stdout, ad);
			}
		}
		result.Close();

		if ( ! fFirstLine) {
			printf("\n");
		}

		delete dae;
	}

	delete query;

	return 0;
}

// return true if p1 is longer than p2 and it ends with p2
// if ppEnd is not NULL, return a pointer to the start of the end of p1
bool ends_with(const char * p1, const char * p2, const char ** ppEnd)
{
	int cch2 = p2 ? strlen(p2) : 0;
	if ( ! cch2)
		return false;

	int cch1 = p1 ? strlen(p1) : 0;
	if (cch2 >= cch1)
		return false;

	const char * p1e = p1+cch1-1;
	const char * p2e = p2+cch2-1;
	while (p2e >= p2) {
		if (*p2e != *p1e)
			return false;
		--p2e; --p1e;
	}
	if (ppEnd)
		*ppEnd = p1e+1;
	return true;
}

// return true if p1 starts with p2
// if ppEnd is not NULL, return a pointer to the first non-matching char of p1
bool starts_with(const char * p1, const char * p2, const char ** ppEnd)
{
	if ( ! p2 || ! *p2)
		return false;

	const char * p1e = p1;
	const char * p2e = p2;
	while (*p2e) {
		if (*p1e != *p2e)
			return false;
		++p2e; ++p1e;
	}
	if (ppEnd)
		*ppEnd = p1e;
	return true;
}

static LOG_INFO * find_log_info(LOG_INFO_MAP & info, std::string name)
{
	LOG_INFO_MAP::const_iterator it = info.find(name);
	if (it == info.end()) {
		return NULL;
	}
	return it->second;
}

static LOG_INFO * find_or_add_log_info(LOG_INFO_MAP & info, std::string name, std::string log_dir)
{
	LOG_INFO * pli = NULL;
	LOG_INFO_MAP::const_iterator it = info.find(name);
	if (it == info.end()) {
		pli = new LOG_INFO();
		pli->name = name;
		pli->log_dir = log_dir;
		info[name] = pli;
		App.all_log_info.Append(pli);
	} else {
		pli = it->second;
	}
	return pli;
}

static void read_address_file(const char * filename, std::string & addr)
{
	addr.clear();
	int fd = safe_open_wrapper_follow(filename, O_RDONLY);
	if (fd < 0)
		return;

	// read the address file into a local buffer
	char buf[4096];
	int cbRead = read(fd, buf, sizeof(buf));

	// parse out the address string. it should be the first line of data
	char * peol = buf;
	while (peol < buf+cbRead) {
		if (*peol == '\r' || *peol == '\n') {
			*peol = 0;
			break;
		}
		++peol;
	}

	close(fd);

	// return the address
	addr = buf;
}

class BWReaderBuffer {

protected:
	char * data;
	int    cbData;
	int    cbAlloc;
	bool   at_eof;
	int    error;

public:
	BWReaderBuffer(int cb=0, char * input = NULL) 
		: data(input), cbData(cb), cbAlloc(cb), at_eof(false), error(0) {
		if (input) {
			cbAlloc = cbData = cb;
		} else if (cb > 0) {
			data = (char*)malloc(cb);
			memset(data, 17, cb);
			cbData = 0;
		}
	}

	~BWReaderBuffer() {
		if (data)
			free(data);
		data = NULL;
		cbData = cbAlloc = 0;
	}

	void clear() { cbData = 0; }
	void setsize(int cb) { cbData = cb; ASSERT(cbData <= cbAlloc); }
	int size() { return cbData; }
	int capacity() { return cbAlloc-1; }
	int LastError() { return error; }
	bool AtEOF() { return at_eof; }
	char operator[](int ix) const { return data[ix]; }
	char& operator[](int ix) { return data[ix]; }

	bool reserve(int cb) {
		if (data && cbAlloc >= cb)
			return true;
		void * pv = realloc(data, cb);
		if (pv) {
			data = (char*)pv;
			cbAlloc = cb;
			return true;
		}
		return false;
	}

	/* returns number of characters read. or 0 on error use LastError & AtEOF methods to know which.
	*/
	int fread_at(FILE * file, off_t offset, int cb) {
		if ( ! reserve(((cb + 16) & ~15) + 16))
			return 0;

		fseek(file, offset, SEEK_SET);

		int ret = fread(data, 1, cb, file);
		cbData = ret;

		if (ret <= 0) {
			error = ferror(file);
			return 0;
		} else {
			error = 0;
		}

		// on windows we can consume more than we read because of \r
		// but since we are scanning backwards this can cause us to re-read
		// the same bytes more than once. So lop off the end of the buffer so
		// so we only get back the unique bytes
		at_eof = feof(file);
		if ( ! at_eof) {
			off_t end_offset = ftell(file);
			int extra = (int)(end_offset - (offset + ret));
			ret -= extra;
		}

		if (ret < cbAlloc) {
			data[ret] =  0; // force null terminate.
		} else {
			// this should NOT happen
			EXCEPT("BWReadBuffer is unexpectedly too small!");
		}

		return ret;
	}
};

class BackwardsReader {
protected:
	int error;
	FILE * file;
	filesize_t cbFile;
	off_t      cbPos;
	BWReaderBuffer buf;

public:
	BackwardsReader(std::string filename, int open_flags) 
		: error(0), file(NULL), cbFile(0), cbPos(0) {
		int fd = safe_open_wrapper_follow(filename.c_str(), open_flags);
		if (fd < 0)
			error = errno;
		else
			OpenFile(fd, "rb");
	}
	BackwardsReader(int fd, const char * open_options) 
		: error(0), file(NULL), cbFile(0), cbPos(0) {
		OpenFile(fd, open_options);
	}
	~BackwardsReader() {
		if (file) fclose(file);
		file = NULL;
	}

	int  LastError() { return error; }
	bool AtEOF() { 
		if ( ! file || (cbPos >= cbFile))
			return true; 
		return false;
	}
	bool AtBOF() { 
		if ( ! file || (cbPos == 0))
			return true; 
		return false;
	}

#if 0
	bool NextLine(std::string & str) {
		// write this
		return false;
	}
#endif
	bool PrevLine(std::string & str) {
		str.clear();

		// can we get a previous line out of our existing buffer?
		// then do that.
		if (PrevLineFromBuf(str))
			return true;

		// no line in the buffer? then return false
		if (AtBOF())
			return false;

		const int cbBack = 128;
		while (true) {
			int off = cbPos > cbBack ? cbPos - cbBack : 0;
			int cbToRead = (int)(cbPos - off);

			// in order to get EOF to register, we have to read a little past the end of file.
			bool expect_eof = false;
			if (cbFile == cbPos) {
				if (!(cbBack & (cbBack-1))) { // cbBack is a power of 2 
					off = cbFile & ~(cbBack-1) - cbBack;
					cbToRead = cbFile - off;
				}
				cbToRead += 16;
				expect_eof = true;
			}

			if ( ! buf.fread_at(file, off, cbToRead)) {
				if (buf.LastError()) {
					error = buf.LastError();
					return false;
				}
			}

			cbPos = off;

			// try again to get some data from the buffer
			if (PrevLineFromBuf(str) || AtBOF())
				return true;
		}
	}

private:
	bool OpenFile(int fd, const char * open_options) {
		file = fdopen(fd, open_options);
		if ( ! file) {
			error = errno;
		} else {
			// seek to the end of the file.
			fseek(file, 0, SEEK_END);
			cbFile = cbPos = ftell(file);
			error = 0;
		}
		return error != 0;
	}

	// prefixes or part of a line into str, and updates internal
	// variables to keep track of what parts of the buffer have been returned.
	bool PrevLineFromBuf(std::string & str)
	{
		// if we have no buffered data, then there is nothing to do
		int cb = buf.size();
		if (cb <= 0)
			return false;

		// if buffer ends in a newline, convert it to a \0
		if (buf[--cb] == '\n') {
			buf[cb] = 0;
		}
		// because of windows style \r\n, we also tolerate a \r at the end of the line
		if (buf[cb-1] == '\r') {
			buf[--cb] = 0;
		}

		// now we walk backward through the buffer until we encounter another newline
		// returning all of the characters that we found.
		while (cb > 0) {
			if (buf[--cb] == '\n') {
				str.insert(0, &buf[cb+1]);
				buf[cb] = 0;
				buf.setsize(cb);
				return true;
			}
		}

		// we hit the start of the buffer without finding another newline,
		// so return that text, but only return true if we are also at the start
		// of the file.
		str.insert(0, &buf[0]);
		buf[0] = 0;
		buf.clear();

		return (0 == cbPos);
	}
};

static void scan_logs_for_info(LOG_INFO_MAP & info, MAP_TO_PID & job_to_pid)
{
	// scan the masterlog backwards until we find the STARTING UP banner
	// looking for places where the log mentions the PIDS of the daemons.
	//
	if (info.find("Master") != info.end()) {
		LOG_INFO * pliMaster = info["Master"];
		std::string filename;
		sprintf(filename, "%s%c%s", pliMaster->log_dir.c_str(), DIR_DELIM_CHAR, pliMaster->log.c_str());
		if (App.diagnostic) printf("scanning master log file '%s' for pids\n", filename.c_str());

		BackwardsReader reader(filename, O_RDONLY);
		if (reader.LastError()) {
			// report error??
			if (App.diagnostic) {
				fprintf(stderr,"Error opening %s: %s\n", filename.c_str(), strerror(reader.LastError()));
			}
			return;
		}

		std::string possible_master_pid;
		std::string line;
		while (reader.PrevLine(line)) {
			// printf("%s\n", line.c_str());

			if (line.find("** condor_master (CONDOR_MASTER) STARTING UP") != string::npos) {
				if (App.diagnostic) {
					printf("found master startup banner with pid %s\n", possible_master_pid.c_str());
					printf("quitting scan of master log file\n\n");
				}
				if (pliMaster->pid.empty())
					pliMaster->pid = possible_master_pid;
				break;
			}
			// parse master header
			unsigned int ix = line.find(" ** PID = ");
			if (ix != string::npos) {
				possible_master_pid = line.substr(ix+10);
			}

			// parse "Sent signal NNN to DAEMON (pid NNNN)"
			ix = line.find("Sent signal");
			if (ix != string::npos) {
				unsigned int ix2 = line.find(" to ", ix);
				if (ix2 != string::npos) {
					ix2 += 4;
					unsigned int ix3 = line.find(" (pid ", ix2);
					if (ix3 != string::npos) {
						unsigned int ix4 = line.find(")", ix3);
						std::string daemon = line.substr(ix2, ix3-ix2);
						std::string pid = line.substr(ix3+6, ix4-ix3-6);
						lower_case(daemon);
						daemon[0] = toupper(daemon[0]);
						if (App.diagnostic)
							printf("From Sent Signal: %s = %s\n", daemon.c_str(), pid.c_str());
						if (info.find(daemon) != info.end()) {
							LOG_INFO * pliDaemon = info[daemon];
							if (pliDaemon->pid.empty())
								info[daemon]->pid = pid;
						}
					}
				}
				continue;
			}

			// parse "Started DaemonCore process PATH, pid and pgroup = NNNN"
			ix = line.find("Started DaemonCore process");
			if (ix != string::npos) {
				unsigned int ix2 = line.find(", pid and pgroup = ", ix);
				if (ix2 != string::npos) {
					std::string pid = line.substr(ix2+sizeof(", pid and pgroup = ")-1);
					unsigned int ix3 = line.rfind("condor_");
					if (ix3 > ix && ix3 < ix2) {
						unsigned int ix4 = line.find_first_of("\".", ix3);
						if (ix4 > ix3 && ix4 < ix2) {
							std::string daemon = line.substr(ix3+7,ix4-ix3-7);
							lower_case(daemon);
							daemon[0] = toupper(daemon[0]);
							if (App.diagnostic)
								printf("From Started DaemonCore process: %s = %s\n", daemon.c_str(), pid.c_str());
							if (info.find(daemon) != info.end()) {
								LOG_INFO * pliDaemon = info[daemon];
								if (pliDaemon->pid.empty())
									info[daemon]->pid = pid;
							}
						}
					}
				}
			}
		}
	}

	// scan the Starter.slotNN logs backwards until we find the STARTING UP banner
	// looking for the starter PIDS, and job PIDS & job IDs
	for (LOG_INFO_MAP::const_iterator it = info.begin(); it != info.end(); ++it)
	{
		if ( ! starts_with(it->first.c_str(),"Slot",NULL)) {
			continue;
		}

		LOG_INFO * pliDaemon = it->second;

		std::string filename;
		sprintf(filename, "%s%c%s", pliDaemon->log_dir.c_str(), DIR_DELIM_CHAR, pliDaemon->log.c_str());
		if (App.diagnostic) printf("scanning %s log file '%s' for pids\n", it->first.c_str(), filename.c_str());

		BackwardsReader reader(filename, O_RDONLY);
		if (reader.LastError()) {
			// report error??
			if (App.diagnostic) {
				fprintf(stderr,"Error opening %s: %s\n", filename.c_str(), strerror(reader.LastError()));
			}
			return;
		}

		std::map<int,bool> dead_pids;
		std::string possible_starter_pid;
		std::string possible_job_id;
		std::string possible_job_pid;

		std::string line;
		while (reader.PrevLine(line)) {
			// printf("%s\n", line.c_str());
			if (line.find("**** condor_starter (condor_STARTER) pid") != string::npos &&
				line.find("EXITING WITH STATUS") != string::npos) {
				if (App.diagnostic) {
					printf("found EXITING line for starter\nquitting scan of %s log file\n\n", it->first.c_str());
				}
				break;
			}
			if (line.find("** condor_starter (CONDOR_STARTER) STARTING UP") != string::npos) {
				if (App.diagnostic) {
					printf("found startup banner with pid %s\n", possible_starter_pid.c_str());
					printf("quitting scan of %s log file\n\n", it->first.c_str());
				}
				if (pliDaemon->pid.empty())
					pliDaemon->pid = possible_starter_pid;
				break;
			}

			// parse start banner
			unsigned int ix = line.find(" ** PID = ");
			if (ix != string::npos) {
				possible_starter_pid = line.substr(ix+10);
			}

			ix = line.find("DaemonCore: command socket at");
			if (ix != string.npos) {
				pliDaemon->addr = line.substr(line.find("<", ix));
			}

			// parse jobid
			ix = line.find("Starting a ");
			if (ix != string::npos) {
				unsigned int ix2 = line.find("job with ID: ", ix);
				if (ix2 != string::npos) {
					possible_job_id = line.substr(line.find(": ", ix2)+2);
					if (App.diagnostic) { printf("found JobId %s\n", possible_job_id.c_str()); }
					if ( ! possible_job_pid.empty()) {
						unsigned int pid = atoi(possible_job_pid.c_str());
						if (App.diagnostic) { printf("Adding %s = %s to job->pid map\n", possible_job_id.c_str(), possible_job_pid.c_str()); }
						job_to_pid[possible_job_id] = pid;
					}
				}
			}

			// collect job pids
			//
			ix = line.find("Create_Process succeeded, pid=");
			if (ix != string::npos) {
				possible_job_pid = line.substr(line.find("=",ix)+1);
				if (App.diagnostic) { printf("found JobPID %s\n", possible_job_pid.c_str()); }
				unsigned int pid = atoi(possible_job_pid.c_str());
				std::map<int,bool>::iterator it_pids = dead_pids.find(pid);
				if (it_pids != dead_pids.end()) {
					possible_job_pid.clear();
					dead_pids.erase(it_pids);
				} else {
					// running_pids.push_back(pid);
				}
			}

			ix = line.find("Process exited, pid=");
			if (ix != string::npos) {
				std::string exited_pid = line.substr(line.find("=",ix)+1);
				if (App.diagnostic) { printf("found PID exited %s\n", exited_pid.c_str()); }
				unsigned int pid = atoi(exited_pid.c_str());
				dead_pids[pid] = true;
			}
		}
	}

}

static void query_log_dir(const char * log_dir, LOG_INFO_MAP & info)
{

	Directory dir(log_dir);
	dir.Rewind();
	while (const char * file = dir.Next()) {

		// figure out what kind of file this is.
		//
		int filetype = 0; // nothing
		std::string name;
		const char * pu = NULL;
		if (file[0] == '.' && ends_with(file, "_address", &pu)) {
			name.insert(0, file+1, pu - file-1);
			name[0] = toupper(name[0]); // capitalize it.
			filetype = 1; // address
			//if (App.diagnostic) printf("\t\tAddress file: %s\n", name.c_str());
		} else if (ends_with(file, "Log", &pu)) {
			name.insert(0, file, pu-file);
			if (name == "Sched") name = "Schedd"; // the schedd log is called CredLog
			if (name == "Start") name = "Startd"; // the startd log is called StartLog
			if (name == "Cred") name = "Credd"; // the credd log is called CredLog
			if (name == "Kbd") name = "Kbdd"; // the kbdd log is called KbdLog
			filetype = 2; // log
		} else if (ends_with(file, "Log.old", &pu)) {
			name.insert(0, file, pu-file);
			if (name == "Sched") name = "Schedd"; // the schedd log is called CredLog
			if (name == "Start") name = "Startd"; // the startd log is called StartLog
			if (name == "Cred") name = "Credd";
			if (name == "Kbd") name = "Kbdd";
			filetype = 3; // previous.log
		} else if (starts_with(file, "StarterLog.", &pu)) {
			const char * pname = pu;
			if (ends_with(pname, ".old", &pu)) {
				name.insert(0, pname, pu - pname);
				filetype = 3; // previous slotlog
			} else {
				name.insert(0, pname);
				filetype = 2; // slotlog
			}
			name[0] = toupper(name[0]);
		}
		if ( ! filetype)
			continue;

		const char * fullpath = dir.GetFullPath();
		//if (App.diagnostic) printf("\t%d %-12s %s\n", filetype, name.c_str(), fullpath);

		if (filetype > 0) {
			LOG_INFO * pli = find_or_add_log_info(info, name, log_dir);
			switch (filetype) {
				case 1: // address file
					read_address_file(fullpath, pli->addr);
					//if (App.diagnostic) printf("\t\t%-12s addr = %s\n", name.c_str(), pli->addr.c_str());
					break;
				case 2: // log file
					pli->log = file;
					//if (App.diagnostic) printf("\t\t%-12s log = %s\n", name.c_str(), pli->log.c_str());
					break;
				case 3: // old log file
					pli->log_old = file;
					//if (App.diagnostic) printf("\t\t%-12s old = %s\n", name.c_str(), pli->log_old.c_str());
					break;
			}
		}
	}
}

void print_log_info(LOG_INFO_MAP & info)
{
	printf("%-12s %-8s %-24s %s\n", "Daemon", "PID", "Addr", "Log, Log.Old");
	printf("%-12s %-8s %-24s %s\n", "------", "---", "----", "---, -------");
	for (LOG_INFO_MAP::const_iterator it = info.begin(); it != info.end(); ++it)
	{
		LOG_INFO * pli = it->second;
		printf("%-12s %-8s %-24s %s, %s\n", 
			it->first.c_str(), pli->pid.c_str(), pli->addr.c_str(), 
			pli->log.c_str(), pli->log_old.c_str());
	}
}

// make a DC config val query for a particular daemon.
static char * get_daemon_param(std::string addr, char * param_name)
{
	char * value = NULL;

	Daemon dae(DT_ANY, addr.c_str(), addr.c_str());

	ReliSock sock;
	sock.timeout(20);   // years of research... :)
	sock.connect(addr.c_str());

	dae.startCommand(DC_CONFIG_VAL, &sock, 2);

	sock.encode();
	//if (App.diagnostic) { printf("Querying %s for $(%s) param\n", addr.c_str(), param_name); }

	if ( ! sock.code(param_name)) {
		if (App.diagnostic) { fprintf( stderr, "Can't send DC_CONFIG_VAL for %s to %s\n", param_name, addr.c_str() ); }
	} else if ( ! sock.end_of_message()) {
		if (App.diagnostic) { fprintf( stderr, "Can't send end of message to %s\n", addr.c_str() ); }
	} else {
		sock.decode();
		if ( ! sock.code(value)) {
			if (App.diagnostic) { fprintf( stderr, "Can't receive reply from %s\n", addr.c_str() ); }
		} else if( ! sock.end_of_message()) {
			if (App.diagnostic) { fprintf( stderr, "Can't receive end of message from %s\n", addr.c_str() ); }
		} else if (App.diagnostic) {
			printf("DC_CONFIG_VAL %s, %s = %s\n", addr.c_str(), param_name, value);
		}
	}

	sock.close();
	return value;
}

#if 0
static char * query_a_daemon2(std::string addr, std::string /*name*/)
{
	char * value = NULL;
	Daemon dae(DT_ANY, addr.c_str(), addr.c_str());

	ReliSock sock;
	sock.timeout(20);   // years of research... :)
	sock.connect(addr.c_str());
	char * param_name = "PID";
	dae.startCommand(DC_CONFIG_VAL, &sock, 2);
	sock.encode();
	if (App.diagnostic) {
		printf("Querying %s for $(%s) param\n", addr.c_str(), param_name);
	}
	if ( ! sock.code(param_name)) {
		if (App.diagnostic) fprintf( stderr, "Can't send request (param_name) to %s\n", param_name, addr.c_str() );
	} else if ( ! sock.end_of_message()) {
		if (App.diagnostic) fprintf( stderr, "Can't send end of message to %s\n", addr.c_str() );
	} else {
		sock.decode();
		char * val = NULL;
		if ( ! sock.code(val)) {
			if (App.diagnostic) fprintf( stderr, "Can't receive reply from %s\n", addr.c_str() );
		} else if( ! sock.end_of_message()) {
			if (App.diagnostic) fprintf( stderr, "Can't receive end of message from %s\n", addr.c_str() );
		} else if (App.diagnostic) {
			printf("Recieved %s from %s for $(%s)\n", val, addr.c_str(), param_name);
		}
		if (val) {
			value = strdup(val);
			free(val);
		}
	}
	sock.end_of_message();
	sock.close();
	return value;
}
#endif

//  use DC_CONFIG_VAL command to fill in daemon PIDs
//
static void query_daemons_for_pids(LOG_INFO_MAP & info)
{
	for (LOG_INFO_MAP::iterator it = info.begin(); it != info.end(); ++it) {
		LOG_INFO * pli = it->second;
		if (pli->name == "Kbdd") continue;
		if (pli->pid.empty() && ! pli->addr.empty()) {
			char * pid = get_daemon_param(pli->addr, "PID");
			if (pid) {
				pli->pid = pid;
				free(pid);
			}
		}
	}
}


static void query_daemons_for_config_built_ins(LOG_INFO_MAP & info)
{
	for (LOG_INFO_MAP::iterator it = info.begin(); it != info.end(); ++it) {
		LOG_INFO * pli = it->second;
		if (pli->name == "Kbdd") continue;
		if ( ! pli->addr.empty()) {
			char * foo = get_daemon_param(pli->addr, "PID"); if (foo) free (foo);
			foo = get_daemon_param(pli->addr, "PPID"); if (foo) free (foo);
			foo = get_daemon_param(pli->addr, "REAL_UID"); if (foo) free (foo);
			foo = get_daemon_param(pli->addr, "REAL_GID"); if (foo) free (foo);
			foo = get_daemon_param(pli->addr, "USERNAME"); if (foo) free (foo);
			foo = get_daemon_param(pli->addr, "HOSTNAME"); if (foo) free (foo);
			foo = get_daemon_param(pli->addr, "FULL_HOSTNAME"); if (foo) free (foo);
			foo = get_daemon_param(pli->addr, "TILDE"); if (foo) free (foo);
		}
	}
}


static bool ping_a_daemon(std::string addr, std::string /*name*/)
{
	bool success = false;
	Daemon dae(DT_ANY, addr.c_str(), addr.c_str());

	ReliSock sock;
	sock.timeout(20);   // years of research... :)
	sock.connect(addr.c_str());
	dae.startCommand(DC_NOP, &sock, 20);
	success = sock.end_of_message();
	sock.close();
	return success;
}

static void ping_all_known_addrs(LOG_INFO_MAP & info)
{
	for (LOG_INFO_MAP::const_iterator it = info.begin(); it != info.end(); ++it) {
		LOG_INFO * pli = it->second;
		if ( ! pli->addr.empty()) {
			ping_a_daemon(pli->addr, it->first);
		}
	}
}
