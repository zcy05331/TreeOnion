#!/usr/bin/env Rscript
args <- commandArgs(trailingOnly=TRUE)
stopifnot(length(args)==1)
out <- args[1]
d <- read.csv(file.path(out,'stages.csv'), check.names=FALSE)
v <- read.csv(file.path(out,'h3-savings.csv'), check.names=FALSE)
groups <- c('frontend','scan','finalize','fold','rotate_project','pack_switch','other')
labs <- c('Query frontend','Scan','CRT / inverse NTT','Fold','Rotate / project','Pack / switch','Other')
cols <- c('#2864AA','#23A893','#80C9BF','#EFAB47','#B478BD','#D76569','#CDD2D8')
models <- c('Standard','Generic h3','Generic h1','Layerwise tuned','Tree g32','Cuckoo tuned','Flat')
basepar <- function() par(family='sans', las=1, bty='n', mgp=c(2.4,.6,0), tcl=-.2)
pdf(file.path(out,'stage-share.pdf'),width=13,height=7,useDingbats=FALSE)
layout(matrix(c(1,2,3,3),2,2,byrow=TRUE),heights=c(9,1.3))
par(mar=c(4,8,3,3));basepar()
for(scale in c('1gb','4gb')) {
  x <- d[d$scale==scale & d$scheme %in% models,]
  x <- x[match(rev(models),x$scheme),]
  m <- t(as.matrix(x[,groups]));m[m<0] <- 0
  m <- sweep(m,2,x$total_ms,'/')*100
  y <- barplot(m,horiz=TRUE,col=cols,border=NA,names.arg=ifelse(x$scheme=='Standard','Standard (block)',x$scheme),xlim=c(0,125),cex.names=.86,axes=FALSE,main=paste(ifelse(scale=='1gb','1 GiB','4 GiB'),' | 32 B entries'))
  axis(1,at=seq(0,100,25),labels=paste0(seq(0,100,25),'%'))
  text(103,y,labels=sprintf('%.3f s',x$total_ms/1000),adj=0,cex=.78,col='#333333')
  mtext('Share of mean server time',side=1,line=2.3,cex=.85)
}
par(mar=c(0,0,0,0));plot.new()
legend('center',legend=labs,fill=cols,ncol=4,bty='n',cex=.85)
dev.off()

pdf(file.path(out,'h3-h1-absolute.pdf'),width=12,height=5.5,useDingbats=FALSE)
layout(matrix(c(1,2,3,3),2,2,byrow=TRUE),heights=c(8,1.5))
par(mar=c(4,5,3,2));basepar()
for(scale in c('1gb','4gb')) {
  x <- d[d$scale==scale & d$scheme %in% c('Generic h1','Generic h3'),]
  x <- x[match(c('Generic h3','Generic h1'),x$scheme),]
  m <- t(as.matrix(x[,groups]));m[m<0] <- 0
  y <- barplot(m,horiz=TRUE,col=cols,border=NA,names.arg=x$scheme,xlim=c(0,max(x$total_ms)*1.18),cex.names=.85,main=ifelse(scale=='1gb','1 GiB','4 GiB'))
  text(x$total_ms+max(x$total_ms)*.025,y,labels=sprintf('%.1f',x$total_ms),adj=0,cex=.83)
  mtext('Mean server time (ms)',side=1,line=2.3,cex=.9)
}
par(mar=c(0,0,0,0));plot.new()
legend('center',legend=labs,fill=cols,ncol=4,bty='n',cex=.82)
dev.off()

pdf(file.path(out,'h3-stage-savings.pdf'),width=13.5,height=5.8,useDingbats=FALSE)
par(mfrow=c(1,2),mar=c(4,8,3,3.5),oma=c(0,0,0,0));basepar()
for(scale in c('1gb','4gb')) {
  x <- v[v$scale==scale,];x <- x[match(rev(groups),x$stage),]
  lim <- range(c(x$saved_ms,0));pad <- diff(lim)*.2
  y <- barplot(x$saved_ms,horiz=TRUE,col=ifelse(x$saved_ms>=0,'#179D89','#D9773B'),border=NA,names.arg=rev(labs),xlim=c(lim[1]-pad,lim[2]+pad),cex.names=.84,main=paste(ifelse(scale=='1gb','1 GiB','4 GiB'),sprintf(' | net %.1f ms',sum(x$saved_ms))))
  abline(v=0,col='#69717D',lwd=.6)
  for(i in seq_along(y)) text(x$saved_ms[i]+ifelse(x$saved_ms[i]>=0,pad*.08,-pad*.08),y[i],labels=sprintf('%+.1f',x$saved_ms[i]),adj=ifelse(x$saved_ms[i]>=0,0,1),cex=.82,xpd=NA)
  mtext('h1 time - h3 time (ms); positive favors h3',side=1,line=2.4,cex=.83)
}
dev.off()
