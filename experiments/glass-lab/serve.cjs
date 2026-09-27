// Local preview only: serve the single experiment page, never the project tree.
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const output=path.resolve(__dirname,'../../artifacts/glass-lab');fs.mkdirSync(output,{recursive:true});
const server=http.createServer((req,res)=>{if(!['/','/index.html'].includes(new URL(req.url,'http://localhost').pathname)){res.writeHead(404);res.end();return;}
 res.writeHead(200,{'Content-Type':'text/html; charset=utf-8','Cache-Control':'no-store'});res.end(fs.readFileSync(path.join(__dirname,'index.html')));});
server.listen(0,'127.0.0.1',()=>{const state={pid:process.pid,url:`http://127.0.0.1:${server.address().port}/`,purpose:'EdgeTuck local material experiment'};fs.writeFileSync(path.join(output,'server.json'),JSON.stringify(state,null,2));console.log(state.url);});
