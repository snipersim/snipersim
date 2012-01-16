<< data.m

data = Select[ data, Length[#] > 0 & ];

(* Normalize to seconds *)
data = Map[ Map[ { #[[1]] / 10^6, #[[2]] } &, # ] &, data ];

If[Length[data] == 0, Print["ERROR: No data to plot."]; Exit[]]

plotStyles = Array[ AbsolutePointSize[ If[ # == 1, 1.0, 1.0] ] &, Length[data] ];

plot = ListPlot[ data, AxesOrigin->{data[[1,1,1]], 0}, PlotStyle->plotStyles,
               Frame->{{True,False},{True,False}},
               FrameLabel->{"Run-time (secs)","Cycles (simulated)"},
               Axes->False,
               BaseStyle->{FontSize->16}];

Export[ "progress_trace.png", plot, ImageResolution->100 ];
