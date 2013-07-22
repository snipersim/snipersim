var colors = {
		"ILP": "blue", 
		"TLP": "red", 
		"Memory": "pink", 
		"Branch": "gray", 
		"PercentOfDataUtilized": "purple",
		"Vectorization":"yellow",
		"NonFP":"green"
	};

//fill global optimization info
	function fillOptimizationInfo(){
	    var num_rows = top_optimizations.length;
	    var theader = '<p>Top '+num_rows+' overall optimizations</p><p>&#160;</p><table class="opttable">\n';
	    var tbody = '';

	    tbody += '<td class="number"></td>';
	    tbody += '<td class="function"><b>Function</b></td>';
	    tbody += '<td class="optimization" align="left"><b>Optimization</b></td>';
	    tbody += '<td class="optimizationbar"></td>';
            tbody += '<td class="speedup"><b>Application speedup</b></td>';
	    tbody += '<td class="buttons"></td>';

	    for( var i=0; i<num_rows;i++)
	    {

		tbody += '<tr>';
		    tbody += '<td class="number">#'+(i+1)+'</td>';
		    tbody += '<td class="function"><div onclick="goToFunctionInfo('+top_optimizations[i]["id"]+')" title="Click to show function info" style="cursor: pointer;">'
				+shortenString(top_optimizations[i]["name_clean"],50)+'</div></td>';
	            tbody += '<td class="optimization" title="'+optimization_summary[top_optimizations[i]["optimization"][0]["optimization"]]["full_name"]+'">';
		    tbody += top_optimizations[i]["optimization"][0]["optimization"]+'</td>';
		    tbody += '<td class="optimizationbar"><div id="overalloptplaceholder'+i+'" style="width:300px;height:15px;top:5px""></div></td>';
		    tbody += '<td class="speedup">';
		    tbody += 'x'+top_optimizations[i]["app_speedup"].toFixed(3)+'</td>';
		    tbody += '<td class="buttons">';                    
		    tbody += '<input type="button" onclick="goToInstrVsTime('+top_optimizations[i]["id"]+')" class="button_instrvstime"'+ 
						'title="Show instructions versus time plot"></input>';
	            tbody += '<input type="button" onclick="goToRoofline('+top_optimizations[i]["id"]+')" class="button_roofline"'+ 
						'title="Show roofline plot"></input>';
		    tbody += '<input type="button" onclick="goToFunctionInfo('+top_optimizations[i]["id"]+')" class="button_info" title="Show function info"></input>';
		    if(getDoxygenFile(top_optimizations[i]["function"]).length > 0){
		    	tbody += '<input type="button" class="button_doxy" title="View DoxyGen information" ';
			tbody += 'onclick="showDoxygen(\''+top_optimizations[i]["function"]+'\')"</input>';
		    }
		    tbody += '</td>';
		tbody += '</tr>\n';
	    }
	    var tfooter = '</table>';
	    document.getElementById('top_optimizationtable').innerHTML = theader + tbody + tfooter;
	    fillOverallOptPlaceholders();
	}

	function fillOverallOptPlaceholders(){
		
		plotoptions = { 
			series: {
				stack: false,
				lines: { show:false },
				bars: { show: true, barWidth: 0.6, horizontal:true }
			}, 
			xaxis: { show: false},
			yaxis: {show: false},
			grid: {show: false,
				hoverable: true, 
				clickable: true, 
			},
			legend: {show: false}  
		};
		
		var normalizedbarlength = 325;
		var normalizedvalue = top_optimizations[0]["app_speedup"]-1+0.000000000001;

		
		for(var overallopt=0; overallopt<top_optimizations.length; overallopt++){
			
			var optname = top_optimizations[overallopt]["optimization"][0]["optimization"];
			var value = top_optimizations[overallopt]["app_speedup"]-1;
			var barlength = normalizedbarlength*(value/normalizedvalue);

			

			var Optbar = {
				color: colors[optname],
				label: overallopt,
				data : [[10,0]]
			     };

			document.getElementById('overalloptplaceholder'+overallopt).style.width=barlength+"px";
			$.plot($("#overalloptplaceholder"+overallopt), [Optbar], plotoptions);
			$("#overalloptplaceholder"+overallopt).bind("plothover", function (event, pos, item) {
				if (item) {
				    if (previousSeries != item.seriesIndex) {
					previousSeries = item.seriesIndex;
					$("#tooltip").remove();
					var x = item.datapoint[0].toFixed(2),
					y = item.datapoint[1].toFixed(2);
					var optindex = item.series["label"];
					var optimization = optimization_summary[top_optimizations[optindex]["optimization"][0]["optimization"]]["full_name"]
					var timewonback = (top_optimizations[optindex]["time_won_back_pct"]).toFixed(2)+"%";
					var timegain = prettyNumber(top_optimizations[optindex]["time_won_back"].toFixed(0))+" ns";
					var speedup = "x"+top_optimizations[optindex]["app_speedup"].toFixed(3);
					showTooltip(item.pageX,item.pageY,"<table>"+
						"<tr><td>Optimization</td><td>"+optimization+"</td></tr>"+
						"<tr><td>Time gain (function %) </td><td align='right'>"+timewonback+"</td></tr>"+
						"<tr><td>Time won back</td><td align='right'>"+timegain+"</td></tr>"+
						"<tr><td>Speedup</td><td align='right'>"+speedup+"</td></tr></table>"
						
						);
					}
				} 
				else {
					$("#tooltip").remove();
					previousSeries = null;            
				}
	

			
		    	});
		};
		

	}



	function fillOptimizationInfoCombined(){
	    var num_rows = optimizationscombined.length;
	    var theader = '<p>Top '+num_rows+' combined optimizations</p><p>&#160;</p><table class="opttable">\n';
	    var tbody = '';

	    tbody += '<td class="number"></td>';
	    tbody += '<td class="function"><b>Function</b></td>';
	    tbody += '<td class="combinedoptimizationbar"><b>Optimization</b></td>';
            tbody += '<td class="speedup"><b>Application speedup</b></td>';
	    tbody += '<td class="buttons"></td>';

	    for( var i=0; i<num_rows;i++)
	    {
		tbody += '<tr>';
		    tbody += '<td class="number">#'+(i+1)+'</td>';
		    tbody += '<td class="function"><div onclick="goToFunctionInfo('+optimizationscombined[i]["id"]+')" title="Show function info" style="cursor: pointer;">'
				+shortenString(optimizationscombined[i]["name_clean"],50)+'</div></td>';
	            tbody += '<td class="combinedoptimizationbar">';
		    tbody += '<div id="optplaceholder'+i+'" style="width:100px;height:15px;top:5px"></div></td>';
		    tbody += '<td class="speedup">';
		    tbody += 'x'+optimizationscombined[i]["app_speedup"].toFixed(3)+'</td>';
		    tbody += '<td class="buttons">';                    
		    tbody += '<input type="button" onclick="goToInstrVsTime('+optimizationscombined[i]["id"]+')" class="button_instrvstime"'+ 
						'title="Show instructions versus time plot"></input>';
	            tbody += '<input type="button" onclick="goToRoofline('+optimizationscombined[i]["id"]+')" class="button_roofline"'+ 
						'title="Show roofline plot"></input>';
		    tbody += '<input type="button" onclick="goToFunctionInfo('+optimizationscombined[i]["id"]+')" class="button_info" title="Show function info"></input>';
		    if(getDoxygenFile(optimizationscombined[i]["function"]).length > 0){
		    	tbody += '<input type="button" class="button_doxy" title="View DoxyGen information" ';
			tbody += 'onclick="showDoxygen(\''+optimizationscombined[i]["function"]+'\')"</input>';
		    }
		    tbody += '</td>';
		tbody += '</tr>\n';

	    }
	    var tfooter = '</table>';
	    document.getElementById('optimizationscombined').innerHTML = theader + tbody + tfooter;
	    fillOptPlaceHolders();

	}

       function fillOptPlaceHolders(){

		var normalizedbarlength = 425;
		var normalizedvalue = optimizationscombined[0]["app_speedup"]-1+0.000000001;
		

		plotoptions = { 
			series: {
				stack: true,
				lines: { show:false },
				bars: { show: true, barWidth: 0.6, horizontal:true }
			}, 
			xaxis: { show: false},
			yaxis: {show: false},
			grid: {show: false,
				hoverable: true, 
				clickable: true, 
			},
			legend: {show: false}  
		};

		for (opt=0; opt < optimizationscombined.length; opt++) {

		   var value = optimizationscombined[opt]["app_speedup"]-1;
		   var barlength = normalizedbarlength*(value/normalizedvalue);

		    var ILP = {
			color: colors["ILP"],
			label: "ILP:"+opt,
			data : [[0,0]]
		     };

		    var TLP = {
			color: colors["TLP"],
			label: "TLP:"+opt,
			data : [[0,0]]
		     };

		     var Memory = {
			color: colors["Memory"],
			label: "Memory:"+opt,
			data : [[0,0]]
		     };

		     var Branch = {
			color: colors["Branch"],
			label: "Branch:"+opt,
			data : [[0,0]]
		     };

		     var PercentOfDataUtilized = {
			color: colors["PercentOfDataUtilized"],
			label: "Percent of data utilized:"+opt,
			data : [[0,0]]
		     };

		     var Vectorization = {
			color: colors["Vectorization"],
			label: "Vectorization:"+opt,
			data : [[0,0]]
		     };

		     var NonFP = {
			color: colors["NonFP"],
			label: "Remove Non-FP instructions:"+opt,
			data : [[0,0]]
		     };

		      totaltimegain = optimizationscombined[opt]["time_won_back"];	

		      for (var prop=0; prop < optimizationscombined[opt]["optimization"].length; prop++){
			optname = optimizationscombined[opt]["optimization"][prop]["optimization"];
			timewonback = optimizationscombined[opt]["optimization"][prop]["timegain"];
			if(optname=="TLP"){
                        	TLP["data"][0]= [((timewonback/totaltimegain)*100),0];
				TLP["label"]+=(":"+prop);
			}
			else if (optname=="ILP"){
                        	ILP["data"][0]= [((timewonback/totaltimegain)*100),0];
				ILP["label"]+=(":"+prop);
			}
			else if (optname=="NonFP"){
                        	NonFP["data"][0]= [((timewonback/totaltimegain)*100),0];
				NonFP["label"]+=(":"+prop);
			}
			else if (optname=="Vectorization"){				
                        	Vectorization["data"][0]= [((timewonback/totaltimegain)*100),0];
				Vectorization["label"]+=(":"+prop);
			}
			else if (optname=="PercentOfDataUtilized"){
                        	PercentOfDataUtilized["data"][0]= [((timewonback/totaltimegain)*100),0];
				PercentOfDataUtilized["label"]+=(":"+prop);
			}
			else if (optname=="Branch"){
                        	Branch["data"][0]= [((timewonback/totaltimegain)*100),0];
				Branch["label"]+=(":"+prop);
			}
			else if (optname=="Memory"){
                        	Memory["data"][0]= [((timewonback/totaltimegain)*100),0];
				Memory["label"]+=(":"+prop);
			}
		      }
		    
			document.getElementById('optplaceholder'+opt).style.width=barlength+"px";
			$.plot($("#optplaceholder"+opt), [TLP, ILP, NonFP, Vectorization, PercentOfDataUtilized, Branch, Memory ], plotoptions);
			previousSeries=null;
			$("#optplaceholder"+opt).bind("plothover", function (event, pos, item) {
				if (item) {
				    if (previousSeries != item.seriesIndex) {
					previousSeries = item.seriesIndex;
					$("#tooltip").remove();
					var x = item.datapoint[0].toFixed(2),
					y = item.datapoint[1].toFixed(2);
					var optimization = item.series["label"].split(":")[0];
					var optindex = item.series["label"].split(":")[1];
					var propindex = item.series["label"].split(":")[2];
					//global stats					
					var timewonback = prettyNumber(optimizationscombined[optindex]["time_won_back"].toFixed(0))+" ns";
					var timegainpct = (optimizationscombined[optindex]["time_won_back_pct"]).toFixed(2)+'%';
					var speedup = "x"+optimizationscombined[optindex]["app_speedup"].toFixed(3);
					//per optimization
				        var timewonbackperopt = prettyNumber(optimizationscombined[optindex]["optimization"][propindex]["timegain"].toFixed(0))+" ns";
					
										

					showTooltip(item.pageX,item.pageY,"<table>"+
						"<tr><td>Optimization</td><td align='right'>"+optimization+"</td></tr>"+
						"<tr><td>Time won back</td><td align='right'>"+timewonbackperopt+"</td></tr>"+
						"<tr><td>Combined with others:</td><td></td></tr>"+
						"<tr><td>Time gain (function %) </td><td align='right'>"+timegainpct+"</td></tr>"+
						"<tr><td>Time won back</td><td align='right'>"+timewonback+"</td></tr>"+
						"<tr><td>Speedup</td><td align='right'>"+speedup+"</td></tr>"+
						"</table>"	
						);	
					}
					
				} 
				else {
					$("#tooltip").remove();
					previousSeries = null;            
				}
			
		    	});

		}

	}




	function showDoxygen(functionname){
		links = getDoxygenFile(functionname);
		for (i=0; i<links.length; i++){
			window.open(links[i]);
		}
	}


	//make optimizationtabs
	function makeTabs() {
		tabcontent='<a id="Tab1" href="javascript:tabChange(1)" title="All optimizations">Overall</a>';
		tabcontent+='<a id="Tab2" href="javascript:tabChange(2)" title="Combined">Combined</a>';
		for (var i=0; i < optimization_names.length; i++){
			tabcontent+='<a id="Tab'+(i+3)+'" href="javascript:tabChange('+(i+3)+')" title="'+optimization_summary[optimization_names[i]]["full_name"]+
					'">'+optimization_names[i]+'</a>';
		}
		document.getElementById('tabs').innerHTML = tabcontent;
		fillTabs();
	}

	function fillTabs() {

		//make divs
		optimizationtabscontent="";
		for (var i=0; i<optimization_names.length; i++){
			optimizationtabscontent+='<div id="Page'+(i+3)+'" style:"display: none;">'+optimization_names[i]+'</div>';
		}
		document.getElementById('optimizationtabs').innerHTML = optimizationtabscontent;
		
		//fill divs
			//for each optimization module
		for (i=0; i<optimization_names.length; i++){
			    var num_rows = optimizationspermodule[i].length;
			    var theader = '<p>Top '+num_rows+' '+optimization_names[i]+' optimizations</p>';
			    theader+='<p>'+optimization_summary[optimization_names[i]]["summary"]+'</p><table class="opttable">';
			    var tbody = '';

			    tbody += '<td class="number"></td>';
			    tbody += '<td class="function"><b>Function</b></td>';
			    tbody += '<td class="combinedoptimizationbar"></td>';
			    tbody += '<td class="speedup"><b>Application speedup</b></td>';
			    tbody += '<td class="buttons"></td>';
			    


			    for( var j=0; j<num_rows;j++)
			    {
				tbody += '<tr>';
				    tbody += '<td class="number">#'+(j+1)+'</td>';
				    tbody += '<td class="function"><div style="cursor: pointer;" onclick="goToFunctionInfo('+optimizationspermodule[i][j]["id"]+')"' 
				    tbody += 'title="Show function info">'+shortenString(optimizationspermodule[i][j]["name_clean"],40)+'</div></td>';
				    tbody += '<td class="combinedoptimizationbar"><div id="peroptplaceholder'+i+'a'+j+'" style="width:100px;height:15px;top:5px"></div></td>';
				    tbody += '<td class="speedup">';
				    tbody += 'x'+optimizationspermodule[i][j]["app_speedup"].toFixed(3)+'</td>';
				    tbody += '<td class="buttons">';				    
				    tbody += '<input type="button" onclick="goToInstrVsTime('+optimizationspermodule[i][j]["id"]+')" class="button_instrvstime"'+ 'title="Show instructions versus time plot"></input>';
				    tbody += '<input type="button" onclick="goToRoofline('+optimizationspermodule[i][j]["id"]+')" class="button_roofline"'+ 
						'title="Show roofline plot"></input>';
				    tbody += '<input type="button" onclick="goToFunctionInfo('+optimizationspermodule[i][j]["id"]+')" class="button_info"'+ 
						'title="Show function info"></input>';
				    if(getDoxygenFile(optimizationspermodule[i][j]["function"]).length > 0){
				    	tbody += '<input type="button" class="button_doxy" title="View DoxyGen information" ';
					tbody += 'onclick="showDoxygen(\''+optimizationspermodule[i][j]["function"]+'\')"</input>';
				    }
				    tbody += '</td>';
				tbody += '</tr>\n';
				
				
			    }
			    var tfooter = '</table>';
			    document.getElementById('Page'+(i+3)).innerHTML = theader + tbody + tfooter;
		}
		tabChange(1);
		fillBars();

	}

	function fillBars(){

		plotoptions = { 
			series: {
				stack: false,
				lines: { show:false },
				bars: { show: true, barWidth: 0.6, horizontal:true }
			}, 
			xaxis: { show: false},
			yaxis: {show: false},
			grid: {show: false,
				hoverable: true, 
				clickable: true, 
			},
			legend: {show: false}  
		};

		var maxbarvalue = 425;

		for (var optname=0; optname<optimization_names.length; optname++){
			var num_rows = optimizationspermodule[optname].length;
			var normalizedvalue = optimizationspermodule[optname][0]["app_speedup"]-1+0.00000000001;
			for( var j=0; j<num_rows;j++){
				
				var Optbar = {
					color: colors[optimizationspermodule[optname][0]["optimization"][0]["optimization"]],
					label: optname+":"+j,
					data : [[10,0]]
				     };
				var barlength = maxbarvalue*((optimizationspermodule[optname][j]["app_speedup"]-1)/normalizedvalue);
				if (barlength == 0) barlength = 1;
				document.getElementById('peroptplaceholder'+optname+'a'+j).style.width=barlength+"px";
				$.plot($("#peroptplaceholder"+optname+"a"+j), [Optbar], plotoptions);
				previousSeries=null;
				$("#peroptplaceholder"+optname+"a"+j).bind("plothover", function (event, pos, item) {
					if (item) {
					    if (previousSeries != item.seriesIndex) {
						previousSeries = item.seriesIndex;
						$("#tooltip").remove();
						var x = item.datapoint[0].toFixed(2),
						y = item.datapoint[1].toFixed(2);
						var optindex = item.series["label"].split(":")[0];
						var optindexj = item.series["label"].split(":")[1];
						var optimization = optimizationspermodule[optindex][0]["optimization"][0]["optimization"];
						var timewonback = (optimizationspermodule[optindex][optindexj]["time_won_back_pct"]).toFixed(2)+"%";
						var timegain = prettyNumber(optimizationspermodule[optindex][optindexj]["time_won_back"].toFixed(0))+" ns";
						var speedup = "x"+optimizationspermodule[optindex][optindexj]["app_speedup"].toFixed(3);
						showTooltip(item.pageX,item.pageY,"<table>"+
							"<tr><td>Optimization</td><td align='right'>"+optimization+"</td></tr>"+
							"<tr><td>Time gain (function %) </td><td align='right'>"+timewonback+"</td></tr>"+
							"<tr><td>Time won back</td><td align='right'>"+timegain+"</td></tr>"+
							"<tr><td>Speedup</td><td align='right'>"+speedup+"</td></tr></table>"
						
							);
						}
					} 
					else {
						$("#tooltip").remove();
						previousSeries = null;            
					}
	

			
			    	});

			}

		}
		


	};


				/*
				
*/


	//change tabs in optimization view
	function tabChange(i) {
	      for ( var j = 1; j <= optimization_names.length+2; j++) {
		   if (i == j) {
		        document.getElementById("Page" + j).style.display = "block";
		        document.getElementById("Tab" + j).className = "tabSelect";
		   } else {
		        document.getElementById("Page" + j).style.display = "none";
		        document.getElementById("Tab" + j).className = "tab";
		   }
	      }
	}

	function shortenString(string, limit)
	{
	  var dots = "...";
	  if(string.length > limit){
	    string = string.substring(0,limit) + dots;
	  } 
	    return string;
	}

        function goToRoofline(functionid){
		window.location.hash="roofline"; 
		if(functionid > 0){
			getInfo(functionid);
		}
		else{
			getInfo(currentfunctionid);
		}
		if ($('#container2').is(':hidden')) {
			$("#container2").slideToggle('slow', function() {});
		}	
	}

	function goToInstrVsTime(functionid){
		window.location.hash="instrvstime"; 
		if(functionid > 0){
			getInfo(functionid);
		}
		else{
			getInfo(currentfunctionid);
		}
		if ($('#container1').is(':hidden')) {
			$("#container1").slideToggle('slow', function() {});
		}		
	}
