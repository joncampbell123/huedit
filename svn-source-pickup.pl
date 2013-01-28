#!/usr/bin/perl

# make sure the build tree is clean
$x = system("./svn-add-all");
die unless $x == 0;

$project = "docs";
if ($project eq "") {
	$i = rindex($url,'/');
	if ($i < 0) {$i = 0;}
	else {$i++;}
	$project = substr($url,$i);
}

if (!open(S,"svn info --non-interactive |")) { exit 1; }
my $lcrev = "x";
my $i,$lcdate = "unknown";
foreach my $line (<S>) {
	chomp $line;
	my @nv = split(/: +/,$line);
	my $value = $nv[1];
	my $name = $nv[0];

	if ($name =~ m/^Last Changed Rev$/i && $lcrev eq "x") {
		$lcrev = int($value);
	}
	elsif ($name =~ m/^Revision$/i && $lcrev eq "x") {
		$lcrev = int($value);
	}
	elsif ($name =~ m/^Last Changed Date$/i) {
		my @b = split(/ +/,$value);
		my $date = $b[0];
		my $time = $b[1];

		$date =~ s/\-//g;
		$time =~ s/\://g;
		$lcdate = $date."-".$time;
	}
}
close(S);

#my $filename = $project."-rev-".sprintf("%08u",$lcrev)."-src.tar.bz2";
my $filename = "../".$project."-rev-".sprintf("%08u",$lcrev)."-src.tar";
if (!( -f "$filename.xz" )) {
	print "Packing source (all build files except LIB,OBJ,etc.)\n";
	print "  to: $filename\n";

	$x = system("tar --exclude=.svn -C .. -cvf $filename $project");
	die unless $x == 0;
	print "Packing to XZ\n";
	$x = system("xz -6e $filename");
	die unless $x == 0;
}

