numPlotPoints = 1000;
movingAverageSize = 5;

<< data.m

data = Select[ data, Length[#] > 0 & ];

(* Normalize to seconds *)
data = Map[ Map[ { #[[1]] / 10^6, #[[2]] } &, # ] &, data ];

If[Length[data] == 0, Print["ERROR: No data to plot."]; Exit[]]

(* Compute means *)

data = Flatten[ data, 1 ]; (* Join into a single list *)
data = Sort[data]; (* Sort data *)

movingAverage[list_, window_] :=
    Module[{average},
        average[list2_, index_, length_] :=
           Module[{size},
                  size = Min[ window, index-1, length-index ];
                  Mean[ Take[ list2, {index - size, index + size} ] ] ];
       Table[ average[list, i, Length[list]], {i, Length[list]} ] ]

times = data[[All,2]];
meanTimes = movingAverage[ times, movingAverageSize ];
diffs = Table[ { data[[i, 1]], times[[i]] - meanTimes[[i]] }, {i, Length[times]} ];

(* Split list into bins *)

maxTime = data[[-1,1]];
timeInterval = maxTime / numPlotPoints;
quotientF[x_] = Quotient[ #[[1]], x ] &;
splitF = GatherBy[ #, quotientF[ timeInterval ] ] &;
bins = splitF[ diffs ];

(* Find statistics *)

binsSim = Map[ #[[2]] &, bins, {2} ]; (* Get rid of real-time *)
mins = Map[ Min, binsSim ];
maxs = Map[ Max, binsSim ];

binsReal = Map[ #[[1]] &, bins, {2} ]; (* Get rid of sim-time *)
realTimes = Map[ Mean, binsReal ];

(* Plot *)
minPlot = Table[ { realTimes[[i]], mins[[i]] }, {i, Length[realTimes]} ];
maxPlot = Table[ { realTimes[[i]], maxs[[i]] }, {i, Length[realTimes]} ];

plot = ListPlot[ {minPlot, maxPlot}, Joined->True,
               AxesOrigin->{0, 0},
               Frame->{{True,False},{True,False}},
               FrameLabel->{"Run-time (secs)","Deviation (simulated cycles)"},
               Axes->False,
               BaseStyle->{FontSize->16}];

Export[ "progress_trace_envelope.png", plot, ImageResolution->100 ];
