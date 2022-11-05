function writeHeader(currentView)
{
  document.write(

'<div id="container">'+
'    <div id="header">'+
'        <table width="100%"><tr><td width="100px">'+
'        <img width="75px" src="../../images/sniperlogo.png"/>'+
'        </td>'+
'        <td>'+
'        <h1>'+
           title+
'        </h1>'+
'        </td>'+
'        <td class="basicstats" width="350px" align="right">'+formatBasicStats()+'</td>'+
'        </tr></table>'+
'    </div>'+
'    <div id="navigation">'+
'      <ul>'+
(use_level2 ? "<li><a href=\"../../levels/level2/cyclestacks.html\">Cycle stacks over time</a></li>" : "<li class='unavailable'>Cycle stacks over time (not available, level2.py did not run)</li>") +
(use_mcpat ? "<li><a href=\"../../levels/level2/mcpatviz.html\">McPAT visualizations over time</a></li>" : "<li class='unavailable' title='Use viz.py --mcpat to enable'>No McPAT</li>") +
(use_level3 ? "<li><a href=\"../../levels/level3/level3.html\">3D (time-cores-IPC) visualization</a></li>" : "<li class='unavailable'>No 3D (time-cores-IPC)</li>") +
(use_topo ? "<li><a href=\"../../levels/topology/topology.html\">Topology</a></li>" : "<li class='unavailable'>No topology</li>") +
(use_profile ? "<li><a href=\"../../levels/profile/profile.html\">Profile</a></li>" : "<li class='unavailable'>No profile</li>") +
(use_aso ? "<li><a href=\"../../levels/functionbased/functionbased.html\">Suggestions for Optimization</a></li>" : "<li class='unavailable' title='Use --viz-aso to enable'>No Suggestions</li>") +
'      </ul>'+
'      <div style="clear:both"></div>'+
'    </div>'+
'</div>'+
'<div id="errorDisplay" hidden="true">'+
'  Error: Unable to fetch simulation data.</br>'+
'  Either the data doesn\'t exist, or you are using a browser like Chrome which disables local file access.</br>'+
'  To fix this issue, either restart Chrome with the "--disable-web-security" option,</br>'+
'  or create a local webserver. For example, one could run "python -m SimpleHTTPSever" in the viz directory.'+
'</div>'
);

}

function formatBasicStats()
{
  return basicstats["html"];
}
