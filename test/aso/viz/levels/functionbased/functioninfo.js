//show info about a function
	var showinfofunction = function (event, pos, item) {
			
		if (item && item.series["label"] == "functions") {			
				getInfo(item.dataIndex);
		}			
	};
	
	//load doxygen tag file if available
	var xmlDoc;
	function loadDoxyIndex(){
		try{
			if (window.XMLHttpRequest)
			  {// code for IE7+, Firefox, Chrome, Opera, Safari
			  xmlhttp=new XMLHttpRequest();
			  }
			else
			  {// code for IE6, IE5
			  xmlhttp=new ActiveXObject("Microsoft.XMLHTTP");
			  }
			xmlhttp.open("GET","doxygen/doxygen.tag",false);
			xmlhttp.send();
			var parser = new DOMParser();
                        xmlDoc = parser.parseFromString(xmlhttp.responseText, "application/xml");

		}
		//when doxygen.tag is not found
		catch(e){
			xmlDoc=null;
		}

	}
	loadDoxyIndex();

	//load doxygen information if available
	function getDoxygenFile(functionname){			
		link=[];
		if(xmlDoc !== null){
		x=xmlDoc.getElementsByTagName("name");
			for (i=0;i<x.length;i++){
				if(x[i].firstChild.nodeValue == functionname){
					anchorfile = x[i].parentNode.getElementsByTagName("anchorfile")[0].firstChild.nodeValue;
					anchor = x[i].parentNode.getElementsByTagName("anchor")[0].firstChild.nodeValue;
					link.push("doxygen/html/"+anchorfile+"#"+anchor);
				}
			}
		}
		return link;
	}
	
	//return number with comma separators
	function numberWithCommas(x) {
    		return x.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ",");
	}

	//write info in the function info div
	function getInfo(functionindex){

		plot1.unhighlight(0,currentfunctionid);
		overviewplot1.unhighlight(0,currentfunctionid);
		plot2.unhighlight(0,currentfunctionid);
		overviewplot2.unhighlight(0,currentfunctionid);
		//now highlight new point
		plot1.highlight(0, functionindex); 
		overviewplot1.highlight(0, functionindex);
		plot2.highlight(0, functionindex); 
		overviewplot2.highlight(0, functionindex);

		createOptimizationTable(functionindex);

		callspercentage = functionpercentages[functionindex]["calls"]*100;
		icountpercentage = functionpercentages[functionindex]["icount"]*100;
		coretimepercentage = functionpercentages[functionindex]["core_elapsed_time"]*100;
		nonidletimepercentage = functionpercentages[functionindex]["nonidle_elapsed_time"]*100;
		fp_addsubpercentage = functionpercentages[functionindex]["fp_addsub"]*100;
		fp_muldivpercentage = functionpercentages[functionindex]["fp_muldiv"]*100;
		l3misspercentage = functionpercentages[functionindex]["l3miss"]*100;
		

		

		//$("#clickdata").hide();
		$("#functionname").text(functions[functionindex]["name_clean"]);
		$("#eip").text("0x"+functions[functionindex]["eip"]);
		$("#source").text(shortenSourcecode(functions[functionindex]["source"]));
		$("#calls").text(numberWithCommas(functions[functionindex]["calls"]));
		$("#callspercentage").text(numberWithCommas(Math.round(callspercentage))+"%");
		$("#icount").text(numberWithCommas(functions[functionindex]["instruction_count"]));
		$("#icountpercentage").text(numberWithCommas(Math.round(icountpercentage))+"%");
		$("#coretime").text(numberWithCommas(Math.round(functions[functionindex]["core_elapsed_time"]))+" ns");
		$("#coretimepercentage").text(numberWithCommas(Math.round(coretimepercentage))+"%");
		$("#nonidletime").text(numberWithCommas(Math.round(functions[functionindex]["nonidle_elapsed_time"]))+" ns");
		$("#nonidletimepercentage").text(numberWithCommas(Math.round(nonidletimepercentage))+"%");
		$("#fp_addsub").text(numberWithCommas(functions[functionindex]["fp_addsub"]));
		$("#fp_addsubpercentage").text(numberWithCommas(Math.round(fp_addsubpercentage))+"%");
		$("#fp_muldiv").text(numberWithCommas(functions[functionindex]["fp_muldiv"]));
		$("#fp_muldivpercentage").text(numberWithCommas(Math.round(fp_muldivpercentage))+"%");
		$("#l3miss").text(numberWithCommas(functions[functionindex]["l3miss"]));
		$("#l3misspercentage").text(numberWithCommas(Math.round(l3misspercentage))+"%");
		$("#cpivalue").text(functions[functionindex]["cpi"].toFixed(3));
		
		$( "#callprogress" ).progressbar({value: callspercentage});
		$( "#icountprogress" ).progressbar({value: icountpercentage});
		$( "#coretimeprogress" ).progressbar({value: coretimepercentage});
		$( "#nonidletimeprogress" ).progressbar({value: nonidletimepercentage});
		$( "#fp_addsubprogress" ).progressbar({value: fp_addsubpercentage});
		$( "#fp_muldivprogress" ).progressbar({value: fp_muldivpercentage});
		$( "#l3missprogress" ).progressbar({value: l3misspercentage});
		
		if ($('#container3').is(':hidden')) {
			$("#container3").slideToggle('slow', function() {});
		}
		if ($("#nextcore_elapsed_time").is(':hidden')){
			for(i=0; i < props.length; i++){
				for (j=0; j<2; j++){
						$("#"+minmax[j]+props[i]).slideToggle('slow', function() {});
						$("#"+nextprev[j]+props[i]).slideToggle('slow', function() {});		
				}
			}
		}


		currentfunctionid=functionindex;
		//currentfunctionname = functions[functionindex]["name"];
		checkMinMax(functionindex);		

		if(getDoxygenFile(functions[functionindex]["name"]).length > 0){
			$("#doxygen").show();
		}
		else {
			$("#doxygen").hide();
		}
		fillCPIBar(functionindex);
		
	}
	

	function fillCPIBar(functionindex){
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

		totaltime	= functions[functionindex]["core_elapsed_time"]*1e6;

		basevalue 	= 100*(functions[functionindex]["cpiBase"])/totaltime;
		branchvalue 	= 100*(functions[functionindex]["cpiBranchPredictor"])/totaltime;
		memvalue	= 100*(functions[functionindex]["cpiMem"])/totaltime;
		othervalue	= 100-basevalue-branchvalue-memvalue;

		var base = {
			color: "red",
			label: "Base",
			data : [[basevalue,0]]
		};

		var branch = {
			color: "yellow",
			label: "Branch",
			data : [[branchvalue,0]]
		};

		var mem = {
			color: "green",
			label: "Memory",
			data : [[memvalue,0]]
		};

		var other = {
			color: "gray",
			label: "Other",
			data : [[othervalue,0]]
		};

		$.plot($("#cpibar"), [base,branch,mem,other ], plotoptions);
		previousSeries=null;
		$("#cpibar").bind("plothover", function (event, pos, item) {
				if (item) {
				    if (previousSeries != item.seriesIndex) {
					previousSeries = item.seriesIndex;
					$("#tooltip").remove();
					var x = item.datapoint[0].toFixed(2),
					y = item.datapoint[1].toFixed(2);
					showTooltip(item.pageX, item.pageY,item.series["label"]+" "+item.series.data[item.dataIndex][0].toFixed(2)+"%");
					}
				} 
				else {
					$("#tooltip").remove();
					previousSeries = null;            
				}
			
		  });


	}

	//shorten de source code filename
	function shortenSourcecode(sourcecode){
		newname = sourcecode.split('/').pop().replace(":0","");
		if (newname == ":0"){
			newname = "";
		}
		return newname;
	}

	//check if we reach the minimum or maximum of a property, to gray out arrows
	function checkMinMax(functionindex){
		for(i=0; i < props.length; i++){
			for (j=0; j<2; j++){
				if(functionindex == functionboundaries[props[i]][minmax[j]]){
					$("#"+minmax[j]+props[i]).animate({"opacity":0.3},"fast", "swing", null);
					$("#"+nextprev[j]+props[i]).animate({"opacity":0.3},"fast", "swing", null);
				}
				else if ($("#"+minmax[j]+props[i]).css('opacity') < 1){
					$("#"+minmax[j]+props[i]).animate({"opacity":1},"fast", "swing", null);
					$("#"+nextprev[j]+props[i]).animate({"opacity":1},"fast", "swing", null);
				}				
			}
		}
	}

	//prettyfy a number (in fs). Output in ns and with comma separators
        function prettyNumber(number){
		number /= 1e6; //to nanoseconds
		pretty = number.toFixed(0); //round
		pretty = numberWithCommas(pretty);
		return pretty;
	}
	
	//create optimizationtable in per function statistics
	function createOptimizationTable(functionid)
	{
	    var num_rows = optimizationsperfunction[functionid].length;
	    var theader = '<table border="0">\n';
	    var tbody = '';

	    tbody += '<td width="150">Optimization</td>';
	    tbody += '<td class="funoptimizationbar"></td>';
            tbody += '<td width="100" align="right" title="Application speedup">Speedup</td>'

	    for( var i=0; i<num_rows;i++)
	    {
		tbody += '<tr>';
		    tbody += '<td title="'+optimization_summary[optimizationsperfunction[functionid][i]["optimization"][0]["optimization"]]["full_name"]+'">';
		    tbody += optimizationsperfunction[functionid][i]["optimization"][0]["optimization"]+'</td>';
		    tbody += '<td class="funoptimizationbar"><div id="finfoopt'+functionid+'a'+i+'" style="width:100px;height:15px;top:5px"></div></td>';
		    tbody += '<td align="right">x'+optimizationsperfunction[functionid][i]["app_speedup"].toFixed(3);
		    tbody += '</td>';
		tbody += '</tr>\n';
		
		
	    }
	    var tfooter = '</table>';
	    document.getElementById('optimizationtable').innerHTML = theader + tbody + tfooter;
	    createOptBar(functionid);
	}


	function createOptBar(functionid){
		var num_rows = optimizationsperfunction[functionid].length;
		var barsize = 380;
		var normalized = optimizationsperfunction[functionid][0]["app_speedup"]-1+0.0000000001;
		for(var index=0; index <num_rows; index++){
			var opt = optimizationsperfunction[functionid][index]["optimization"][0]["optimization"];
			var value = optimizationsperfunction[functionid][index]["app_speedup"]-1;
			var barlength = barsize*(value/normalized);
			document.getElementById("finfoopt"+functionid+"a"+index).style.width=barlength+"px";
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

			var Optbar = {
				color: colors[opt],
				label: functionid+":"+index,
				data : [[10,0]]
			};
			if(barlength > 1){
				$.plot($("#finfoopt"+functionid+"a"+index), [Optbar], plotoptions);
				$("#finfoopt"+functionid+"a"+index).bind("plothover", function (event, pos, item) {
					if (item) {
					    if (previousSeries != item.seriesIndex) {
						previousSeries = item.seriesIndex;
						$("#tooltip").remove();
						var x = item.datapoint[0].toFixed(2),
						y = item.datapoint[1].toFixed(2);
						var funindex = item.series["label"].split(":")[0];
						var optindexj = item.series["label"].split(":")[1];
						var optimization = optimizationsperfunction[funindex][optindexj]["optimization"][0]["optimization"];
						var timewonback = (optimizationsperfunction[funindex][optindexj]["time_won_back_pct"]).toFixed(2)+"%";
						var timegain = prettyNumber(optimizationsperfunction[funindex][optindexj]["time_won_back"].toFixed(0))+" ns";
						var speedup = "x"+optimizationsperfunction[funindex][optindexj]["app_speedup"].toFixed(3);
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
	}

	//to have a nicer line up of the percentages, add a space to percentages < 10
	function addSpace(percentage){
        	if (percentage.length < 5){
			return "<span style='visibility: hidden'>0</span>"+percentage;
		}
		return percentage;
	}


	//hover over function
	var hoverfunction = function (event, pos, item) {
		if (item) {
			if (previousPoint != item.dataIndex) {
				previousPoint = item.dataIndex;
				$("#tooltip").remove();
				var x = item.datapoint[0].toFixed(2),
				y = item.datapoint[1].toFixed(2);
				if(item.series["label"] == "functions"){
					showTooltip(item.pageX, item.pageY,
			   		functions[item.dataIndex]["name_clean"]);
				}
				else {
					showTooltip(item.pageX, item.pageY,
			   		item.series["label"]);
				}
			}
		} 
		else {
				$("#tooltip").remove();
				previousPoint = null;            
		}
		
	};

	//get next function
	function getNext(sortby, next){
		sortedfunctions = functions.slice();
		sortedfunctions.sort(function(x,y) 
			{ 
				return (parseInt(1000*y[sortby]) - parseInt(1000*x[sortby])); 
			});
		i=sortedfunctions.length-1;
		if(next=="prev"){		
			while(i>=0 && (parseInt(1000*sortedfunctions[i][sortby]) < parseInt(1000*functions[currentfunctionid][sortby]))){
				i--;
			}
			if(i!=-1 && i!=sortedfunctions.length-1){
				return sortedfunctions[i+1]["id"];
			}			
		}
		else{
			while(i>0 && (parseInt(1000*sortedfunctions[i][sortby]) <= parseInt(1000*functions[currentfunctionid][sortby]))){
				i--;
			}
			if(i>=0 && i!=sortedfunctions.length-1){
				return sortedfunctions[i]["id"];
			}			
		}
		return currentfunctionid;

	}

        function goToFunctionInfo(functionid){	
		if(functionid > 0){
			getInfo(functionid);
		}
		else{
			getInfo(currentfunctionid);
		}
		window.location.hash="function_info";
	}

