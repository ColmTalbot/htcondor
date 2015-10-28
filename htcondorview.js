var active_filter;
var alt_title;

function toggle_edit(btn, controls) {
	$(controls).toggle();
	if($(controls).is(":visible")) {
		$(btn).text("❌")
	} else {
		$(btn).text("edit")
	}
}

$('#editlinkgraph1').click(function() { toggle_edit('#editlinkgraph1', '#graph1editor'); });
$('#editlinktable1').click(function() { toggle_edit('#editlinktable1', '#table1editor'); });

function load_arguments_to_form() {
	var args = afterquery.parseArgs(window.location.search);

	var source = args.get('source');
	if(source === undefined) { source = 'submitters'; }
	$('#data-source input[type="radio"][value="'+source+'"]').prop('checked', true);

	var duration = args.get('duration');
	if(duration === undefined) { duration = 'day'; }
	$('#data-duration input[type="radio"][value="'+duration+'"]').prop('checked', true);

	var filterstr = args.get('filter');
	if(filterstr !== undefined) {
		var filterlist = filterstr.split(';');
		active_filter = {};
		for(var i = 0; i < filterlist.length; i++) {
			if(filterlist[i].length == 0) { continue; }
			var pair = filterlist[i].split('=');
			active_filter[pair[0]] = pair[1];
		}
	} else {
		active_filter = undefined;
	}

	var title = args.get('title');
	if(title !== undefined) { alt_title = title; }
}

var urlTool = document.createElement('a');
function replace_search_arg(oldurl, newkey, newval) {
	urlTool.href = oldurl;
	var oldsearch = urlTool.search;
	var args = afterquery.parseArgs(oldsearch);
	var newsearch = "?";
	for(var argi in args.all) {
		var arg = args.all[argi];
		var key = arg[0];
		if(key.length && key != newkey) {
			var val = arg[1];
			newsearch += encodeURIComponent(key) + "=" + encodeURIComponent(val) + "&";	
		}
	}
	if(newval !== undefined) {
		newsearch += encodeURIComponent(newkey) + "=" + encodeURIComponent(newval);	
	}
	urlTool.search = newsearch;
	return urlTool.href;
}

function save_arguments_to_url() {
	var url = window.location.href;

	var source = $('#data-source input[type="radio"]:checked').val()
	var duration = $('#data-duration input[type="radio"]:checked').val()

	url = replace_search_arg(url, 'source', source);
	url = replace_search_arg(url, 'duration', duration);

	var filter;
	if(active_filter !== undefined) {
		filter = '';
		for(key in active_filter) {
			filter = filter + key + "=" + active_filter[key] + ";";
		}
		filter = encodeURI(filter);
	}
	url = replace_search_arg(url, 'filter', filter);

	url = replace_search_arg(url, 'title', alt_title);

	if(url != window.location.href) {
		history.pushState(null, null, url);
	}
}

function read_arguments(source) {
  var out = [];
  var parts = $(source).val().trim().split('\n');
  for (var parti in parts) {
    var part = parts[parti];
    if (part) {
      var bits = afterquery.internal.trySplitOne(part, '=');
      if (bits) {
        out.push(encodeURIComponent(bits[0]) + '=' + encodeURIComponent(bits[1]));
      } else {
        out.push(encodeURIComponent(part));
      }
    }
  }
  return  '?' + out.join('&');
}


var data_url;
var data;
function load_and_render() {
	var callback_render_table = function() {
		setTimeout(function() {
			var options = {
				select_handler: table_select_handler,
				num_pattern: '#,##0.0',
				disable_height: true
			};

			afterquery.render(read_arguments('#table1text'), data.value, null, 'table1', options);
		}, 0);
	 };

	var callback_render_graph = function(){
		setTimeout(function() {
			afterquery.render(read_arguments('#graph1text'), data.value, callback_render_table, 'graph1');
			},0)
		};
	var args = read_arguments('#graph1text');
	var newurl = afterquery.parseArgs(args).get('url');
	if(newurl == data_url) {
		callback_render_graph();
	} else {
		data_url = newurl;
		data = afterquery.load(args, null, callback_render_graph, 'graph1');
	}
}

function table_select_handler(evnt,table,data) {
	var selection = table.getSelection();
	if(selection) {
		var source = $('#data-source input[type="radio"]:checked').val()
		var duration = $('#data-duration input[type="radio"]:checked').val()
		if(source == "machines") {
			var row = selection[0].row;
			var arch = data.getValue(row, 0);
			var opsys = data.getValue(row, 1);
			active_filter = {Arch: arch, OpSys: opsys};
			alt_title = "Machine State for "+arch+"/"+opsys;
			var new_graph_args = graph_args(true, source, duration, active_filter, alt_title);
			render_new_graph('#graph1text', 'graph1', new_graph_args);
		} else if(source =="submitters") {
			var row = selection[0].row;
			var user = data.getValue(row, 0);
			active_filter = {Name:user};
			alt_title = "Jobs for "+user;
			var new_graph_args = graph_args(true, source, duration, active_filter, alt_title);
			render_new_graph('#graph1text', 'graph1', new_graph_args);
		}
	}
}

function render_new_graph(editid, graphid, args) {
	if(args && args.length) {
		$(editid).val(args);
	}
	save_arguments_to_url();
	/*afterquery.render(read_arguments(editid), data.value,null,graphid);*/
	var to_prune = "#" + graphid + ' .vizchart';
	$(to_prune).empty();
	/* TODO: use cached data if posible */
	load_and_render();
}

function change_view() {
	var duration = $('#data-duration input[type="radio"]:checked').val()
	var source = $('#data-source input[type="radio"]:checked').val()
	if(source == "machines" || source == "submitters") {
		render_new_graph('#graph1text', 'graph1', graph_args(true, source, duration, active_filter, alt_title));
		render_new_graph('#table1text', 'table1', graph_args(false, source, duration, active_filter, alt_title));
	} else if(source=="custom") {
		$("#graph1 .vizchart").html("<h2>Not yet implemented</h2>");
		$("#table1 .vizchart").html("<h2>Not yet implemented</h2>");
	}
}

function submitters_data_source() { return "submitters.json"; }
function submitters_now_data_source() { return "submitters.now.json"; }
function machines_data_source() { return "machines.json"; }
function machines_now_data_source() { return "machines.now.json"; }

/*
is_chart - true it's a chart (pie/stacked), false it's a table.
source - submitters or machines
duration - now, day, week, or month
filters - optional. Hash of fields to filter on
   mapped to values to filter by.
title - optional title for chart.
*/
function graph_args(is_chart, source, duration, filters, title) {
	var filter = '';
	if(filters !== undefined) {
		var key;
		for(key in filters) {
			filter += "filter=" + key + "=" + filters[key] + "\n";
		}
	}
	switch(source) {
		case 'submitters':
		{
			if(title === undefined) { title = 'Total Jobs'; }
			var charttype = '';
			if(is_chart) { charttype = 'chart=stacked'; }
			if(duration == 'now') {
				var grouping = 'pivot=Name;JobStatus;Count';
				if(is_chart) {
					charttype = 'chart=pie';
					grouping = "group=JobStatus;Count";
				} else {
					filter = '';
				}
				return "title=" + title + "\n" +
					"url=" + submitters_now_data_source() + "\n" +
					filter +
					"order=JobStatus\n" +
					grouping + "\n" +
					charttype + "\n";

			} else {
				var pivot = "Name;JobStatus;avg(Count)";
				if(is_chart) {
					pivot = "Date;JobStatus;Count";
				} else {
					filter = '';
				}
				return "url=" + submitters_data_source() + "\n" +
					"title=" + title + "\n" +
					filter +
					"order=Date\n" +
					"pivot=" + pivot + "\n" +
					charttype + "\n";
			}
			break;
		}
		case 'machines':
		{
			if(title === undefined) { title = 'Machine State'; }
			if(duration == 'now') {
				if(is_chart) {
					return "title="+title+"\n"+
						"url=machines.now.json\n"+
						"order=State\n"+
						"group=State;Cpus\n"+
						"chart=pie";
				} else {
					return "title="+title+"\n"+
						"url=machines.now.json\n"+
						"order=Arch,OpSys\n"+
						"group=Arch,OpSys;State;Cpus";
				}
			
			} else {
				if(is_chart) {
					return "title=" + title + "\n" +
						"url=" + machines_data_source() + "\n" +
						filter +
						"order=Date\n" +
						"pivot=Date;State;Cpus\n" +
						"chart=stacked\n";
				} else {
					return "url=" + machines_data_source() + "\n" +
						"order=Date\n" +
						"pivot=Date,Arch,OpSys;State;Cpus\n" +
						"group=Arch,OpSys;avg(Unclaimed),avg(Claimed),max(Unclaimed),max(Claimed)";
				}
			}
		}
	}
}

function starting_html() {
	return "" +
	"<div style=\"text-align: center\">\n" +
	"<ul class=\"radio-tabs\" id=\"data-source\">\n" +
	"<li><input type=\"radio\" name=\"data-source\" id=\"data-source-user\" value=\"submitters\"> <label for=\"data-source-user\">Users</label>\n" +
	"<li><input type=\"radio\" name=\"data-source\" id=\"data-source-machine\" value=\"machines\"> <label for=\"data-source-machine\">Pool</label>\n" +
	"<li><input type=\"radio\" name=\"data-source\" id=\"data-source-custom\" value=\"custom\"> <label for=\"data-source-custom\">Custom</label>\n" +
	"</ul>\n" +
	"<ul class=\"radio-tabs\" id=\"data-duration\">\n" +
	"<li><input type=\"radio\" name=\"data-duration\" id=\"data-duration-now\" value=\"now\"> <label for=\"data-duration-now\">Now</label>\n" +
	"<li><input type=\"radio\" name=\"data-duration\" id=\"data-duration-day\" value=\"day\"> <label for=\"data-duration-day\">Day</label>\n" +
	"<li><input type=\"radio\" name=\"data-duration\" id=\"data-duration-week\" value=\"week\"> <label for=\"data-duration-week\">Week</label>\n" +
	"<li><input type=\"radio\" name=\"data-duration\" id=\"data-duration-month\" value=\"month\"> <label for=\"data-duration-month\">Month</label>\n" +
	"</ul>\n" +
	"</div>\n" +
	"\n" +
	"<div id=\"tab-user\" class=\"tab-content current\">\n" +
	"\n" +
	"<div class='editmenu'><button class=\"editlink\" id='editlinkgraph1'>edit</button>\n" +
	"<div id=\"graph1editor\" style=\"display:none;\">\n" +
	"<textarea id='graph1text' cols=\"40\" rows=\"10\" wrap='off'>\n" +
	"</textarea>\n" +
	"<div>\n" +
	"<button id=\"rerendergraph1\">Update Graph</button>\n" +
	"<button id=\"reloadgraph1\">Reload Data</button>\n" +
	"</div>\n" +
	"</div>\n" +
	"<br><button onclick=\"alert('Not yet implemented')\" class=\"editlink\">full screen</button>\n" +
	"</div>\n" +
	"\n" +
	"<div id='graph1'>\n" +
	"<div class='vizstatus'>\n" +
	"  <div class='statustext'></div>\n" +
	"  <div class='statussub'></div>\n" +
	"</div>\n" +
	"<div class='vizraw'></div>\n" +
	"<div class='vizchart'></div>\n" +
	"</div>\n" +
	"\n" +
	"<div class=\"download-link\"> <a onclick=\"alert('Not yet implemented');\" href=\"#not-yet-implemented\">Download this table</a> </div>\n" +
	"<div class='editmenu'><button class=\"editlink\" id='editlinktable1'>edit</button>\n" +
	"<div id=\"table1editor\" style=\"display:none;\">\n" +
	"<textarea id='table1text' cols=\"40\" rows=\"10\" wrap='off'>\n" +
	"</textarea>\n" +
	"<div>\n" +
	"<button id=\"rerendertable1\">Update Table</button>\n" +
	"<button id=\"reloadtable1\">Reload Data</button>\n" +
	"</div>\n" +
	"</div>\n" +
	"</div>\n" +
	"\n" +
	"<div id='table1'>\n" +
	"<div class='vizstatus'>\n" +
	"  <div class='statustext'></div>\n" +
	"  <div class='statussub'></div>\n" +
	"</div>\n" +
	"<div class='vizraw'></div>\n" +
	"<div class='vizchart'></div>\n" +
	"</div>\n" +
	"\n" +
	"</div> <!-- #tab-user .tab-content -->\n" +
	"\n" +
	"<div id=\"tab-machine\" class=\"tab-content\">\n" +
	"</div>\n" +
	"\n" +
	"<div id=\"tab-custom\" class=\"tab-content\">\n" +
	"TODO Custom\n" +
	"</div>\n" +
	"\n" +
	"<div id='vizlog'></div>\n" +
	"";
}




function initialize_htcondor_view(id) {
	var container = $('#'+id);
	if(container.length == 0) {
		console.log('HTCondor View is not able to intialize. There is no element with an ID of "'+id+'".');
		return false;
	}
	container.html(starting_html());

	window.onpopstate = function() {
		setTimeout(function(){
			load_arguments_to_form();
			load_and_render();
			},1);
	}

	$('#reloadgraph1').click(function() {
		load_and_render();
		save_arguments_to_url();
		});
	$('#rerendergraph1').click(function() { render_new_graph('#graph1text', 'graph1'); });

	$('#reloadtable1').click(function() {
		load_and_render();
		save_arguments_to_url();
		});
	$('#rerendertable1').click(function() { render_new_graph('#table1text', 'table1'); });

	// Initialize tabs
	$('ul.tabs li').click(function(){
		var tab_id = $(this).attr('data-tab');

		$('ul.tabs li').removeClass('current');
		$('.tab-content').removeClass('current');

		$(this).addClass('current');
		$("#"+tab_id).addClass('current');
	});

	$('ul.radio-tabs input').change(function() {
		active_filter = undefined;
		alt_title = undefined;
		change_view();
		});

	load_arguments_to_form();
	change_view()
	load_and_render();

	return true;
}


